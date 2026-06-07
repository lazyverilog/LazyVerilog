#include "server.hpp"
#include "analyzer.hpp"
#include "string_utils.hpp"
#include "background_compiler.hpp"
#include "config.hpp"
#include "dynamic_file_index.hpp"

// LspCpp headers
#include "LibLsp/JsonRpc/Condition.h"
#include "LibLsp/JsonRpc/Endpoint.h"
#include "LibLsp/JsonRpc/RemoteEndPoint.h"
#include "LibLsp/JsonRpc/stream.h"
#include "LibLsp/lsp/ProtocolJsonHandler.h"
#include "LibLsp/lsp/general/exit.h"
#include "LibLsp/lsp/general/initialize.h"
#include "LibLsp/lsp/general/initialized.h"
#include "LibLsp/lsp/general/lsServerCapabilities.h"
#include "LibLsp/lsp/general/lsTextDocumentClientCapabilities.h"
#include "LibLsp/lsp/general/shutdown.h"
#include "LibLsp/lsp/textDocument/did_change.h"
#include "LibLsp/lsp/textDocument/did_close.h"
#include "LibLsp/lsp/textDocument/did_open.h"
#include "LibLsp/lsp/workspace/did_change_configuration.h"
#include "LibLsp/lsp/workspace/did_change_watched_files.h"
// Feature handlers
#include "LibLsp/JsonRpc/serializer.h"
#include "LibLsp/lsp/textDocument/completion.h"
#include "LibLsp/lsp/textDocument/declaration_definition.h"
#include "LibLsp/lsp/textDocument/formatting.h"
#include "LibLsp/lsp/textDocument/hover.h"
#include "LibLsp/lsp/textDocument/inlayHint.h"
#include "LibLsp/lsp/textDocument/prepareRename.h"
#include "LibLsp/lsp/textDocument/publishDiagnostics.h"
#include "LibLsp/lsp/textDocument/range_formatting.h"
#include "LibLsp/lsp/textDocument/references.h"
#include "LibLsp/lsp/textDocument/foldingRange.h"
#include "LibLsp/lsp/textDocument/rename.h"
#include "LibLsp/lsp/textDocument/signature_help.h"
#include "LibLsp/lsp/windows/MessageNotify.h"
#include "LibLsp/lsp/workspace/execute_command.h"
#include "LibLsp/lsp/workspace/symbol.h"
#include "features/completion.hpp"
#include "features/autoarg.hpp"
#include "features/autoff.hpp"
#include "features/autofunc.hpp"
#include "features/autoinst.hpp"
#include "features/autowire.hpp"
#include "features/code_action.hpp"
#include "features/connect.hpp"
#include "features/definition.hpp"
#include "features/folding_range.hpp"
#include "features/formatter.hpp"
#include "features/hover.hpp"
#include "features/inlay_hints.hpp"
#include "features/lint.hpp"
#include "features/references.hpp"
#include "features/rename.hpp"
#include "features/signature_help.hpp"
#include "features/workspace_symbols.hpp"
#include "syntax_index.hpp"
#include <slang/syntax/SyntaxTree.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <thread>
#include <iostream>
#include <map>
#include <memory>
#include <span>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

DEFINE_REQUEST_RESPONSE_TYPE(wp_inlayHintRefresh, JsonNull, JsonNull,
                             "workspace/inlayHint/refresh");

struct StdOutStream : lsp::base_ostream<std::ostream> {
    explicit StdOutStream() : base_ostream<std::ostream>(std::cout) {}
    std::string what() override { return {}; }
};
struct StdInStream : lsp::base_istream<std::istream> {
    explicit StdInStream() : base_istream<std::istream>(std::cin) {}
    std::string what() override { return {}; }
};

// ── Incremental-sync helpers ──────────────────────────────────────────────────

// Advance past col UTF-16 code units from pos, staying on the current line.
// 4-byte UTF-8 sequences (U+10000+) count as 2 UTF-16 units (surrogate pair).
static size_t advance_utf16_cols(const std::string& text, size_t pos, int col) {
    int units = 0;
    while (pos < text.size() && text[pos] != '\n' && units < col) {
        unsigned char c = static_cast<unsigned char>(text[pos]);
        int bytes, extra;
        if      (c < 0x80)           { bytes = 1; extra = 0; }
        else if ((c & 0xE0) == 0xC0) { bytes = 2; extra = 0; }
        else if ((c & 0xF0) == 0xE0) { bytes = 3; extra = 0; }
        else if ((c & 0xF8) == 0xF0) { bytes = 4; extra = 1; } // surrogate pair
        else                         { bytes = 1; extra = 0; } // continuation/invalid

        // LSP text is specified as UTF-8, but be defensive: never skip past the
        // buffer or across malformed continuation bytes.  Treat malformed
        // sequences as one byte / one UTF-16 unit so incremental sync remains
        // monotonic and cannot jump over unrelated text.
        bool valid_sequence = pos + static_cast<size_t>(bytes) <= text.size();
        for (int i = 1; valid_sequence && i < bytes; ++i) {
            unsigned char cc = static_cast<unsigned char>(text[pos + static_cast<size_t>(i)]);
            valid_sequence = (cc & 0xC0) == 0x80;
        }
        if (!valid_sequence) {
            bytes = 1;
            extra = 0;
        }

        if (units + 1 + extra > col) break;
        units += 1 + extra;
        pos   += bytes;
    }
    return pos;
}

// Convert (line, col) LSP position to byte offset in text.
static size_t lsp_offset(const std::string& text, int line, int col) {
    int cur = 0;
    size_t pos = 0;
    while (pos < text.size() && cur < line) {
        if (text[pos] == '\n')
            ++cur;
        ++pos;
    }
    return advance_utf16_cols(text, pos, col);
}

// Compute two byte offsets in a single scan. Positions must be in document order
// (start before end); if not they are swapped and the result pair is returned
// in swapped order so callers always get (start_offset, end_offset).
static std::pair<size_t, size_t> lsp_offset_pair(const std::string& text,
                                                 int line1, int col1,
                                                 int line2, int col2) {
    // Ensure we scan in document order while preserving the caller-visible
    // ordering at the end.  LSP ranges are normally ordered, but the helper is
    // used by generic range utilities, so accepting reversed positions makes
    // the function defensive.
    bool swapped = (line1 > line2 || (line1 == line2 && col1 > col2));
    if (swapped) {
        std::swap(line1, line2);
        std::swap(col1, col2);
    }

    // Walk to the start of the first requested line once.
    int cur = 0;
    size_t pos = 0;
    while (pos < text.size() && cur < line1) {
        if (text[pos] == '\n')
            ++cur;
        ++pos;
    }
    const size_t line1_start = pos;

    // Important: compute both same-line offsets from the original line start.
    // A previous implementation set the second line start to the already
    // advanced first offset.  For a zero-width same-line insertion at column C,
    // that turned (C, C) into offsets (C, 2*C), causing didChange to delete
    // existing text after the cursor.
    size_t off1 = advance_utf16_cols(text, line1_start, col1);

    if (line1 == line2) {
        size_t off2 = advance_utf16_cols(text, line1_start, col2);
        if (swapped)
            std::swap(off1, off2);
        return {off1, off2};
    }

    // Different-line case: continue scanning from line1 start to line2 start,
    // then compute col2 relative to that true second-line start.
    pos = line1_start;
    while (pos < text.size() && cur < line2) {
        if (text[pos] == '\n')
            ++cur;
        ++pos;
    }
    const size_t line2_start = pos;
    size_t off2 = advance_utf16_cols(text, line2_start, col2);

    if (swapped)
        std::swap(off1, off2);
    return {off1, off2};
}

// Format a generated replacement fragment and strip final newlines so it can be
// used as an LSP TextEdit.newText value for a bounded range.  `format_source()`
// renders complete files with a trailing newline, but range replacements such as
// AutoArg's module-header edit should not accidentally insert an extra blank line
// before the untouched body that follows the edited range.
static std::string format_emit_text(const std::string& text, const FormatOptions& options) {
    std::string formatted = format_source(text, options);
    while (!formatted.empty() && formatted.back() == '\n')
        formatted.pop_back();
    return formatted;
}

static std::string json_string(std::string_view text) {
    std::string out;
    // Most SystemVerilog text does not need escaping.  Reserve the common-case
    // payload plus quotes up front so large formatting/workspace responses do
    // not grow one byte at a time.  Escaped characters can exceed this estimate,
    // but the reserve still removes nearly all reallocations for normal files.
    out.reserve(text.size() + 2 + text.size() / 8);
    out += "\"";
    for (char c : text) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            out += c;
            break;
        }
    }
    out += "\"";
    return out;
}

static lsPosition document_end_position(const DocumentState& state) {
    return lsPosition(state.end_line, state.end_character);
}

static lsTextEdit whole_document_edit(const DocumentState& state, const std::string& new_text) {
    lsTextEdit edit;
    edit.range.start = lsPosition(0, 0);
    edit.range.end = document_end_position(state);
    edit.newText = new_text;
    return edit;
}

static std::string whole_document_workspace_edit_json(const std::string& uri,
                                                      const DocumentState& state,
                                                      const std::string& new_text) {
    const auto end = document_end_position(state);
    const auto escaped_uri = json_string(uri);
    const auto escaped_text = json_string(new_text);
    const auto end_line = std::to_string(end.line);
    const auto end_character = std::to_string(end.character);

    std::string out;
    out.reserve(escaped_uri.size() + escaped_text.size() + end_line.size() +
                end_character.size() + 128);
    out += "{\"changes\":{";
    out += escaped_uri;
    out += ":[{\"range\":{\"start\":{\"line\":0,\"character\":0},\"end\":{\"line\":";
    out += end_line;
    out += ",\"character\":";
    out += end_character;
    out += "}},\"newText\":";
    out += escaped_text;
    out += "}]}}";
    return out;
}

static std::string slice_lsp_range(const std::string& text, const lsRange& range) {
    auto [start, end] = lsp_offset_pair(text,
        range.start.line, range.start.character,
        range.end.line, range.end.character);
    if (start > text.size()) start = text.size();
    if (end > text.size()) end = text.size();
    if (start > end) start = end;
    return text.substr(start, end - start);
}

static lsTextEdit range_format_edit(const std::string& old_text, const std::string& formatted_text,
                                    lsRange range) {
    if (range.end < range.start)
        std::swap(range.start, range.end);
    lsTextEdit edit;
    edit.range = range;
    edit.newText = slice_lsp_range(formatted_text, range);
    return edit;
}

static std::string did_change_config_file(lsp::Any settings_any) {
    // LspCpp stores arbitrary JSON as lsp::Any.  Avoid reflected nested structs
    // here: GetFromMap() is convenient for flat maps, but nested user structs
    // require full Reflect() overloads.  The didChangeConfiguration payload is
    // tiny, so explicit maps are clearer and more robust.
    //
    // Expected shape from lua/lazyverilog/lsp.lua:
    //   { "lazyverilog": { "configFile": "/proj/lazyverilog.toml" } }
    std::map<std::string, lsp::Any> settings;
    try {
        settings_any.Get(settings);
    } catch (...) {
        // Settings are client-provided and optional.  Malformed payloads should
        // not fail configuration reload; fall back to root-based config lookup.
        return {};
    }

    auto lazy_it = settings.find("lazyverilog");
    if (lazy_it == settings.end())
        return {};

    std::map<std::string, lsp::Any> lazyverilog;
    try {
        lazy_it->second.Get(lazyverilog);
    } catch (...) {
        // Same best-effort rule as above: ignore non-object lazyverilog
        // settings rather than rejecting the whole LSP notification.
        return {};
    }

    auto file_it = lazyverilog.find("configFile");
    if (file_it == lazyverilog.end())
        return {};

    std::string config_file;
    try {
        file_it->second.GetForMapHelper(config_file);
    } catch (...) {
        // configFile is a hint from our client.  If it is not a string, the
        // server can still reload from the established workspace root.
        return {};
    }
    return config_file;
}

static std::string workspace_edit_json(const std::string& uri, const lsTextEdit& edit) {
    return "{\"changes\":{" + json_string(uri) +
           ":[{\"range\":{\"start\":{\"line\":" + std::to_string(edit.range.start.line) +
           ",\"character\":" + std::to_string(edit.range.start.character) +
           "},\"end\":{\"line\":" + std::to_string(edit.range.end.line) +
           ",\"character\":" + std::to_string(edit.range.end.character) +
           "}},\"newText\":" + json_string(edit.newText) + "}]}}";
}

static void append_rtl_tree_json(std::string& out, const RtlTreeNode& node, bool show_file,
                                 bool show_instance_name, size_t depth = 0) {
    constexpr size_t kMaxRtlTreeJsonDepth = 512;
    out += "{\"name\":";
    out += json_string(node.name);
    // Always include navigation metadata.  `rtltree.show_file` and
    // `rtltree.show_instance_name` control the rendered label in the client,
    // not whether <CR> can jump to a definition when that label is hidden.
    out += ",\"inst\":";
    out += json_string(node.inst);
    out += ",\"file\":";
    out += json_string(node.file);
    out += ",\"line\":";
    out += std::to_string(node.line);
    out += ",\"col\":";
    out += std::to_string(node.col);
    if (depth == 0) {
        out += ",\"show_file\":";
        out += show_file ? "true" : "false";
        out += ",\"show_instance_name\":";
        out += show_instance_name ? "true" : "false";
    }
    out += ",\"children\":[";
    if (depth < kMaxRtlTreeJsonDepth) {
        for (size_t i = 0; i < node.children.size(); ++i) {
            if (i > 0)
                out += ",";
            append_rtl_tree_json(out, node.children[i], show_file, show_instance_name, depth + 1);
        }
    }
    out += "]";
    if (node.recursive)
        out += ",\"recursive\":true";
    if (node.truncated || depth >= kMaxRtlTreeJsonDepth)
        out += ",\"truncated\":true";
    out += "}";
}

static std::string rtl_tree_json(const RtlTreeNode& node, bool show_file,
                                 bool show_instance_name) {
    std::string out;
    // Start with a modest reserve; recursive appends grow this single buffer
    // instead of allocating and copying one completed child JSON string per
    // hierarchy node.
    out.reserve(1024);
    append_rtl_tree_json(out, node, show_file, show_instance_name);
    return out;
}

static std::string apply_incremental_change(std::string text,
                                            const lsTextDocumentContentChangeEvent& chg) {
    if (!chg.range)
        return chg.text; // full-document replacement fallback
    auto [start, end] = lsp_offset_pair(text,
        chg.range->start.line, chg.range->start.character,
        chg.range->end.line, chg.range->end.character);
    if (start > text.size()) start = text.size();
    if (end > text.size()) end = text.size();
    if (start > end) start = end;
    text.replace(start, end - start, chg.text);
    return text;
}

static std::string resolve_vcode_path(const std::filesystem::path& root, const Config& config) {
    if (config.design.vcode.empty())
        return {};
    auto filelist = std::filesystem::path(config.design.vcode);
    if (filelist.is_relative())
        filelist = root / filelist;
    return std::filesystem::absolute(filelist).lexically_normal().string();
}

static std::string path_to_file_uri(const std::filesystem::path& path) {
    return "file://" + std::filesystem::absolute(path).lexically_normal().string();
}

struct VcodeResult {
    std::vector<std::string> files;
    std::vector<std::string> include_dirs;
};

static VcodeResult load_vcode(const std::filesystem::path& root, const Config& config) {
    VcodeResult result;
    if (config.design.vcode.empty())
        return result;

    const auto filelist = std::filesystem::path(resolve_vcode_path(root, config));
    std::ifstream input(filelist);
    if (!input)
        return result;

    const auto filelist_dir = filelist.parent_path();

    std::string line;
    while (std::getline(input, line)) {
        // Strip // and # comments
        if (auto pos = line.find("//"); pos != std::string::npos)
            line.erase(pos);
        if (auto pos = line.find('#'); pos != std::string::npos)
            line.erase(pos);

        auto item = trim_copy(line);
        if (item.empty())
            continue;

        // Recognize simulator-style include-directory entries in the filelist.
        // The directory is not parsed as a source file; it is passed to slang's
        // SourceManager so explicit source files can resolve `include "..."`.
        //
        // Supported forms:
        //   +incdir+rtl/include
        //   +incdir+/abs/include
        //   +incdir+dir_a+dir_b
        if (item.starts_with("+incdir+")) {
            std::string_view rest(item);
            rest.remove_prefix(std::string_view("+incdir+").size());
            while (!rest.empty()) {
                const auto plus = rest.find('+');
                auto dir_text = plus == std::string_view::npos ? rest : rest.substr(0, plus);
                auto dir = std::filesystem::path(std::string(dir_text));
                if (!dir.empty()) {
                    if (dir.is_relative())
                        dir = filelist_dir / dir;
                    result.include_dirs.push_back(
                        std::filesystem::absolute(dir).lexically_normal().string());
                }
                if (plus == std::string_view::npos)
                    break;
                rest.remove_prefix(plus + 1);
            }
            continue;
        }

        // Skip other compiler options / flags.
        if (item.starts_with("+") || item.starts_with("-"))
            continue;

        // Regular file path
        auto path = std::filesystem::path(item);
        if (path.is_relative())
            path = filelist_dir / path;
        result.files.push_back(std::filesystem::absolute(path).lexically_normal().string());
    }
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────

struct StderrLog : public lsp::Log {
    void log(Level, std::wstring&& msg) override { std::wcerr << msg << L"\n"; }
    void log(Level, const std::wstring& msg) override { std::wcerr << msg << L"\n"; }
    void log(Level, std::string&& msg) override { std::cerr << msg << "\n"; }
    void log(Level, const std::string& msg) override { std::cerr << msg << "\n"; }
};

struct LazyVerilogServer::Impl {
    StderrLog log;
    // Completion providers are immutable after construction.  Keep the engine
    // owned by this server instance instead of using a function-local static
    // (global shared lifetime) or rebuilding providers on every keystroke.
    CompletionEngine completion_engine;
    std::shared_ptr<lsp::ProtocolJsonHandler> json_handler =
        std::make_shared<lsp::ProtocolJsonHandler>();
    std::shared_ptr<GenericEndpoint> endpoint = std::make_shared<GenericEndpoint>(log);
    // max_workers=1: keeps LSP request handling serialized.  This avoids
    // historical allocator/runtime races and keeps mutable server state such as
    // diagnostics/config notifications simple.  Expensive project indexing and
    // optional semantic compilation still run on their own background workers.
    RemoteEndPoint remote_endpoint{json_handler, endpoint, log, lsp::JSONStreamStyle::Standard, 1};
    std::shared_ptr<StdOutStream> output = std::make_shared<StdOutStream>();
    std::shared_ptr<StdInStream> input = std::make_shared<StdInStream>();
    Condition<bool> exit_event;

    // Declared after endpoint state so diag_thread (destroyed first as the
    // last-declared member) joins before remote_endpoint is torn down.
    std::atomic<int> diag_debounce_ms{0};
    std::mutex diag_pending_mutex;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> diag_pending;
    std::condition_variable_any diag_cv;
    std::atomic<bool> diag_stop_{false};
    std::thread diag_thread;

    ~Impl() {
        if (diag_thread.joinable()) {
            {
                std::lock_guard<std::mutex> lock(diag_pending_mutex);
                diag_stop_.store(true);
            }
            diag_cv.notify_all();
            diag_thread.join();
        }
    }
};

LazyVerilogServer::LazyVerilogServer() : impl_(std::make_unique<Impl>()) {
    root_ = std::filesystem::current_path();
    config_found_ = std::filesystem::exists(root_ / "lazyverilog.toml");
    config_ = load_config(root_);
    analyzer_.set_project_index_publish_callback([this] {
        request_inlay_hint_refresh();

        // A project-index publish can change lint results in already-open
        // documents even when the edited file is a different open/filelist
        // buffer.  Example: editing memory.sv removes a port; memory_top.sv is
        // still open and has stale-AutoInst lint enabled.  Re-publish foreground
        // diagnostics for all open documents so those cross-file diagnostics are
        // refreshed as soon as the merged project index catches up.
        std::vector<std::string> open_uris;
        analyzer_.for_each_state(
            [&](const std::string& uri, const std::shared_ptr<const DocumentState>& state) {
                if (state)
                    open_uris.push_back(uri);
            });
        for (const auto& uri : open_uris)
            schedule_diagnostics(uri);
    });
    // Hardcoded 250 ms shard-merge cadence.  This coalesces rapid shard
    // completions without exposing a knob most users never tune.  The
    // user-visible diagnostic latency is controlled by [lint].diagnostic_debounce_ms.
    analyzer_.set_project_index_publish_debounce_ms(250);
    impl_->diag_debounce_ms = config_.lint.diagnostic_debounce_ms;
    impl_->diag_thread = std::thread([this] {
        diag_debounce_loop();
    });
    background_compiler_ = std::make_unique<BackgroundCompiler>(
        [this] { return analyzer_.compilation_snapshot(); },
        [this](BackgroundCompileResult result) {
            std::unordered_set<std::string> uris_to_publish;
            auto publish_if_open = [&](const std::string& uri) {
                if (analyzer_.get_state(uri))
                    uris_to_publish.insert(uri);
            };

            // Semantic compilation can produce diagnostics for every file in
            // the design filelist, but publishing all of those diagnostics can
            // flood clients and burn shared filesystem / UI resources on large
            // HPC projects. Keep the cache complete, but only publish URIs that
            // are currently open in the editor. Open URIs from the just-finished
            // snapshot are included so previous diagnostics can be cleared when
            // a new compile produces no diagnostics for that buffer.
            for (const auto& uri : result.open_uris)
                publish_if_open(uri);
            for (const auto& uri : analyzer_.semantic_diagnostic_uris())
                publish_if_open(uri);
            for (const auto& [uri, _] : result.diagnostics_by_uri)
                publish_if_open(uri);
            analyzer_.set_semantic_diagnostics(std::move(result.diagnostics_by_uri));
            for (const auto& uri : uris_to_publish)
                publish_diagnostics(uri);
        });
    configure_background_compiler();
    schedule_background_compilation();
    register_handlers();
    impl_->remote_endpoint.startProcessingMessages(impl_->input, impl_->output);
}

LazyVerilogServer::~LazyVerilogServer() {
    analyzer_.set_project_index_publish_callback({});
    if (background_compiler_)
        background_compiler_->stop();
}

void LazyVerilogServer::run() { impl_->exit_event.wait(); }

void LazyVerilogServer::request_inlay_hint_refresh() {
    if (!config_.inlay_hint.enable || !impl_)
        return;

    std::lock_guard<std::mutex> outbound_lock(outbound_mutex_);
    try {
        auto req = impl_->remote_endpoint.createRequest<wp_inlayHintRefresh::request>();
        req.params = JsonNull{};
        (void)impl_->remote_endpoint.send(req);
    } catch (const std::exception& e) {
        std::cerr << "[lazyverilog] inlayHint refresh error: " << e.what() << "\n";
    }
}

void LazyVerilogServer::configure_background_compiler() {
    if (!background_compiler_)
        return;

    background_compiler_->configure(BackgroundCompilerConfig{
        .enabled = config_.compilation.background_compilation,
        .thread_count = config_.compilation.background_compilation_threads,
        .debounce_ms = config_.compilation.background_compilation_debounce_ms,
        .log_timing = config_.compilation.log_timing,
        .nice_value = config_.compilation.nice_value,
    });

    if (!config_.compilation.background_compilation)
        analyzer_.clear_all_semantic_diagnostics();
}

void LazyVerilogServer::schedule_background_compilation() {
    if (!background_compiler_ || !config_.compilation.background_compilation)
        return;
    background_compiler_->schedule();
}

void LazyVerilogServer::publish_config_diagnostic(const ConfigWarning* warning) {
    std::lock_guard<std::mutex> outbound_lock(outbound_mutex_);
    try {
        if (!config_diagnostic_uri_.empty()) {
            Notify_TextDocumentPublishDiagnostics::notify clear;
            clear.params.uri.raw_uri_ = config_diagnostic_uri_;
            impl_->remote_endpoint.sendNotification(clear);
            config_diagnostic_uri_.clear();
        }

        if (!warning || warning->message.empty() || warning->path.empty())
            return;

        Notify_TextDocumentPublishDiagnostics::notify notif;
        notif.params.uri.raw_uri_ = path_to_file_uri(warning->path);
        config_diagnostic_uri_ = notif.params.uri.raw_uri_;

        lsDiagnostic diag;
        const auto line = warning->line > 0 ? static_cast<int>(warning->line - 1) : 0;
        const auto column = warning->column > 0 ? static_cast<int>(warning->column - 1) : 0;
        diag.range.start = lsPosition(line, column);
        diag.range.end = lsPosition(line, column + 1);
        diag.severity = lsDiagnosticSeverity::Warning;
        diag.source = std::string("lazyverilog");
        diag.message = warning->message;
        notif.params.diagnostics.push_back(std::move(diag));
        impl_->remote_endpoint.sendNotification(notif);
    } catch (const std::exception& e) {
        std::cerr << "[lazyverilog] config diagnostic error: " << e.what() << "\n";
    }
}

void LazyVerilogServer::publish_diagnostics(const std::string& uri) {
    std::lock_guard<std::mutex> outbound_lock(outbound_mutex_);
    try {
        auto state = analyzer_.get_state(uri);
        std::unordered_map<std::string, std::vector<ParseDiagInfo>> diags_by_uri;
        diags_by_uri[uri];
        if (state) {
            auto add_diag = [&](ParseDiagInfo diag) {
                auto target_uri = diag.uri.empty() ? uri : diag.uri;
                diag.uri.clear();
                diags_by_uri[target_uri].push_back(std::move(diag));
            };
            for (auto diag : state->parse_diagnostics)
                add_diag(std::move(diag));

            // Most lint rules need only the current file's live SyntaxTree.
            // The stale AutoInst check also needs module declarations from
            // closed/project files.  Current/open-file facts are derived from
            // the AST inside run_lint(); closed/project facts come from the
            // latest immutable background-published index snapshot.  Do not
            // synchronously parse, copy, or merge the full design filelist on
            // every edit.
            std::shared_ptr<const ProjectIndexSnapshot> project_lint_index;
            if (config_.lint.module.stale_autoinst_diagnostic)
                project_lint_index = analyzer_.project_index_snapshot();

            auto lint_diags = run_lint(*state, config_.lint, project_lint_index.get());
            for (auto diag : lint_diags)
                add_diag(std::move(diag));
        }

        if (config_.compilation.background_compilation) {
            auto semantic_diags = analyzer_.semantic_diagnostics(uri);
            auto& target = diags_by_uri[uri];
            target.insert(target.end(), semantic_diags.begin(), semantic_diags.end());
        }

        auto& previously_published = diagnostic_uris_by_owner_[uri];
        for (const auto& old_uri : previously_published)
            diags_by_uri.try_emplace(old_uri);
        previously_published.clear();

        for (const auto& [target_uri, all_diags] : diags_by_uri) {
            Notify_TextDocumentPublishDiagnostics::notify notif;
            notif.params.uri.raw_uri_ = target_uri;
            for (const auto& d : all_diags) {
                lsDiagnostic ld;
                ld.range.start = lsPosition(d.line, d.col);
                ld.range.end = ld.range.start;
                switch (d.severity) {
                case 1:
                    ld.severity = lsDiagnosticSeverity::Error;
                    break;
                case 2:
                    ld.severity = lsDiagnosticSeverity::Warning;
                    break;
                default:
                    ld.severity = lsDiagnosticSeverity::Information;
                    break;
                }
                ld.source = std::string("lazyverilog");
                ld.message = d.message;
                notif.params.diagnostics.push_back(std::move(ld));
            }
            impl_->remote_endpoint.sendNotification(notif);
            previously_published.insert(target_uri);
        }
    } catch (const std::exception& e) {
        std::cerr << "[lazyverilog] publishDiagnostics error: " << e.what() << "\n";
    }
}

void LazyVerilogServer::schedule_diagnostics(const std::string& uri) {
    if (impl_->diag_debounce_ms <= 0) {
        publish_diagnostics(uri);
        return;
    }
    {
        std::lock_guard lock(impl_->diag_pending_mutex);
        impl_->diag_pending[uri] = std::chrono::steady_clock::now() +
                                   std::chrono::milliseconds(impl_->diag_debounce_ms);
    }
    impl_->diag_cv.notify_one();
}

void LazyVerilogServer::diag_debounce_loop() {
    using Clock = std::chrono::steady_clock;
    while (!impl_->diag_stop_.load()) {
        std::unique_lock<std::mutex> lock(impl_->diag_pending_mutex);
        impl_->diag_cv.wait(lock, [&] {
            return impl_->diag_stop_.load() || !impl_->diag_pending.empty();
        });
        if (impl_->diag_stop_.load())
            break;

        // Find soonest due time across all pending URIs.
        auto soonest = impl_->diag_pending.begin()->second;
        for (const auto& [u, due] : impl_->diag_pending)
            if (due < soonest) soonest = due;

        if (soonest > Clock::now()) {
            impl_->diag_cv.wait_until(lock, soonest, [&] {
                return impl_->diag_stop_.load();
            });
            continue;
        }

        // Drain all due entries under the lock, then publish without it.
        std::vector<std::string> due_uris;
        const auto now = Clock::now();
        for (auto it = impl_->diag_pending.begin(); it != impl_->diag_pending.end();) {
            if (it->second <= now) {
                due_uris.push_back(it->first);
                it = impl_->diag_pending.erase(it);
            } else {
                ++it;
            }
        }
        lock.unlock();

        for (const auto& uri : due_uris) {
            // If update_text() staged a text-only state (null tree), reparse now.
            auto state = analyzer_.get_state(uri);
            if (state && !state->tree) {
                analyzer_.change(uri, state->text);
                analyzer_.clear_semantic_diagnostics(uri);
                schedule_background_compilation();
            }
            publish_diagnostics(uri);
        }
    }
}

void LazyVerilogServer::clear_published_diagnostics_for_owner(const std::string& owner_uri) {
    std::lock_guard<std::mutex> outbound_lock(outbound_mutex_);
    // LSP diagnostics are client-owned state: once the server has published a
    // diagnostic for a URI, many clients keep displaying it until the same
    // server publishes an empty diagnostics array for that URI.  Closing the
    // owner document removes our local DocumentState, so this helper snapshots
    // every URI that the owner previously published (including include-file /
    // semantic diagnostics routed to a different target URI) before erasing the
    // ownership record.
    std::unordered_set<std::string> uris_to_clear;
    uris_to_clear.insert(owner_uri);

    if (auto it = diagnostic_uris_by_owner_.find(owner_uri);
        it != diagnostic_uris_by_owner_.end()) {
        uris_to_clear.insert(it->second.begin(), it->second.end());
        diagnostic_uris_by_owner_.erase(it);
    }

    for (const auto& target_uri : uris_to_clear) {
        Notify_TextDocumentPublishDiagnostics::notify notif;
        notif.params.uri.raw_uri_ = target_uri;
        // Intentionally leave diagnostics empty: this is the LSP "clear"
        // operation for diagnostics previously published by this server.
        impl_->remote_endpoint.sendNotification(notif);
    }
}

void LazyVerilogServer::register_handlers() {
    auto* remote = &impl_->remote_endpoint;
    auto& ep = *remote;
    auto show_warning = [remote](const std::string& message) {
        Notify_ShowMessage::notify note;
        note.params.type = lsMessageType::Warning;
        note.params.message = message;
        remote->sendNotification(note);
    };

    // ── initialize ────────────────────────────────────────────────────────────
    ep.registerHandler([&, show_warning](const td_initialize::request& req) {
        td_initialize::response rsp;
        rsp.id = req.id;
        try {
            auto& caps = rsp.result.capabilities;

            // Text document sync: incremental + open/close notifications
            lsTextDocumentSyncOptions sync_opts;
            sync_opts.openClose = true;
            sync_opts.change = lsTextDocumentSyncKind::Incremental;
            caps.textDocumentSync = std::make_pair(optional<lsTextDocumentSyncKind>{},
                                                   optional<lsTextDocumentSyncOptions>(sync_opts));

            // Simple bool providers
            caps.hoverProvider = true;
            caps.documentFormattingProvider =
                std::make_pair(optional<bool>(true), optional<WorkDoneProgressOptions>{});
            caps.documentRangeFormattingProvider =
                std::make_pair(optional<bool>(true), optional<WorkDoneProgressOptions>{});
            caps.definitionProvider =
                std::make_pair(optional<bool>(true), optional<WorkDoneProgressOptions>{});
            caps.referencesProvider =
                std::make_pair(optional<bool>(true), optional<WorkDoneProgressOptions>{});
            caps.workspaceSymbolProvider =
                std::make_pair(optional<bool>(true), optional<WorkDoneProgressOptions>{});

            // Completion triggers are intentionally conservative.
            //
            // Earlier versions advertised several very common typing
            // characters, such as `(`, `#`, `"`, and `=`.  That made clients
            // which automatically request completion on trigger characters
            // enter the completion engine during ordinary RTL editing:
            //
            //   u_foo (       // `(` is typed constantly in instantiations/calls
            //   #(.W(32))     // `#` is common in parameterized instantiations
            //   assign a =    // `=` appears in normal expressions
            //   `include "    // `"` appears in strings/includes
            //
            // In HPC/shared-resource environments those extra requests are
            // especially painful because even a cached completion path still
            // burns shared CPU.  Manual completion remains available in all
            // contexts; only automatic trigger-based requests are reduced.
            //
            // Keep only the triggers that strongly indicate a SystemVerilog
            // completion context:
            //   "."  member / named-port style contexts
            //   ":"  package/class scope completion via "::" (LSP triggers are
            //        single strings, so ":" is the practical trigger)
            //   "`"  macro completion
            lsCompletionOptions comp_opts;
            comp_opts.triggerCharacters = std::vector<std::string>{".", ":", "`"};
            comp_opts.resolveProvider = false;
            caps.completionProvider = comp_opts;

            // Signature help: trigger on '(' and ','
            lsSignatureHelpOptions sig_opts;
            sig_opts.triggerCharacters = {"(", ","};
            caps.signatureHelpProvider = sig_opts;

            // Rename: advertise prepareRename support
            RenameOptions rename_opts;
            rename_opts.prepareProvider = true;
            caps.renameProvider =
                std::make_pair(optional<bool>{}, optional<RenameOptions>(rename_opts));

            // Code action
            caps.codeActionProvider =
                std::make_pair(optional<bool>(true), optional<CodeActionOptions>{});

            // Folding range
            caps.foldingRangeProvider =
                std::make_pair(optional<bool>(true), optional<FoldingRangeOptions>{});

            // Extract workspace root from initialize params
            auto uri_to_path = [](const std::string& uri) -> std::filesystem::path {
                if (uri.starts_with("file://"))
                    return {uri.substr(7)};
                return {uri};
            };

            // Apply the workspace root selected by either the modern LSP
            // `rootUri` field or the legacy `rootPath` fallback.  Keep all
            // config/vcode/background-index side effects in one place so a
            // future change (for example, adding another project-level cache or
            // warning diagnostic) cannot accidentally update only one
            // initialize branch.
            auto initialize_workspace_root = [&](const std::filesystem::path& p) {
                if (!std::filesystem::exists(p))
                    return;

                root_ = p;
                config_found_ = std::filesystem::exists(root_ / "lazyverilog.toml");

                std::string warn;
                ConfigWarning warning_detail;
                config_ = load_config(root_, &warn, &warning_detail);
                impl_->diag_debounce_ms = config_.lint.diagnostic_debounce_ms;
                if (!warn.empty())
                    show_warning(warn);
                publish_config_diagnostic(warn.empty() ? nullptr : &warning_detail);

                auto vcode = load_vcode(root_, config_);
                analyzer_.set_project_config(config_.design.define, vcode.include_dirs,
                                             vcode.files, resolve_vcode_path(root_, config_));
                configure_background_compiler();
                schedule_background_compilation();
            };

            if (req.params.rootUri && !req.params.rootUri->raw_uri_.empty()) {
                initialize_workspace_root(uri_to_path(req.params.rootUri->raw_uri_));
            } else if (req.params.rootPath && !req.params.rootPath->empty()) {
                initialize_workspace_root(std::filesystem::path(*req.params.rootPath));
            } else {
                // No workspace root from client; index using the path resolved at
                // construction time.  project config is only applied here (not in
                // the constructor) so there is no wasted first-generation parse
                // when the client does supply a rootUri.
                initialize_workspace_root(root_);
            }

            // Inlay hints
            caps.inlayHintProvider = std::make_pair(optional<bool>(config_.inlay_hint.enable),
                                                    optional<InlayHintOptions>{});

            // Execute command — server-side commands
            lsExecuteCommandOptions exec_opts;
            exec_opts.commands = {
                "lazyverilog.format",
                "lazyverilog.rtlTree",
                "lazyverilog.rtlTreeReverse",
                "lazyverilog.autowire",
                "lazyverilog.autowirepreview",
                "lazyverilog.connectInfo",
                "lazyverilog.connectHierarchyChildren",
                "lazyverilog.connectApply",
                "lazyverilog.connectApplyPreview",
                "lazyverilog.autoffPreview",
                "lazyverilog.autoffApply",
                "lazyverilog.autoffAllPreview",
                "lazyverilog.autoffAllApply",
                "lazyverilog.interface",
                "lazyverilog.interfaceConnect",
                "lazyverilog.interfaceDisconnect",
                "lazyverilog.singleInterface",
                "lazyverilog.lint",
                "lazyverilog.lintAll",
            };
            caps.executeCommandProvider = exec_opts;
        } catch (const std::exception& e) {
            std::cerr << "[lazyverilog] initialize error: " << e.what() << "\n";
        }
        return rsp;
    });

    // ── initialized ───────────────────────────────────────────────────────────
    ep.registerHandler([&](const Notify_InitializedNotification::notify&) {
        // no-op: client signals ready, server can dynamically register here
    });

    // ── shutdown ──────────────────────────────────────────────────────────────
    ep.registerHandler([&](const td_shutdown::request& req) {
        td_shutdown::response rsp;
        rsp.id = req.id;
        // LSP spec: shutdown result must be null, not omitted
        lsp::Any null_result;
        null_result.SetJsonString("null", lsp::Any::kNullType);
        rsp.result = std::move(null_result);
        return rsp;
    });

    // ── exit ──────────────────────────────────────────────────────────────────
    ep.registerHandler([&, remote](const Notify_Exit::notify&) {
        try {
            remote->stop();
        } catch (const std::exception& e) {
            std::cerr << "[lazyverilog] endpoint stop during exit failed: " << e.what() << "\n";
        } catch (...) {
            std::cerr << "[lazyverilog] endpoint stop during exit failed\n";
        }
        impl_->exit_event.notify(std::make_unique<bool>(true));
    });

    // ── workspace/didChangeConfiguration ─────────────────────────────────────
    ep.registerHandler(
        [&, show_warning](const Notify_WorkspaceDidChangeConfiguration::notify& note) {
            try {
                // Prefer the explicit config path supplied by our Neovim
                // client.  This avoids stale formatter options when Neovim and
                // the server disagree about the workspace root:
                //
                //   client root: /repo              (because .git was found)
                //   config file: /repo/rtl/lazyverilog.toml
                //   server root: /repo              (from initialize rootUri)
                //
                // If we ignored configFile, reload would keep reading
                // /repo/lazyverilog.toml (or defaults) forever.  The payload is
                // only a hint for selecting the root; load_config() still reads
                // and validates lazyverilog.toml from disk.
                {
                    std::string config_file = did_change_config_file(note.params.settings);
                    if (!config_file.empty()) {
                        if (config_file.starts_with("file://"))
                            config_file = config_file.substr(7);
                        std::filesystem::path config_path(config_file);
                        if (config_path.is_relative())
                            config_path = root_ / config_path;
                        if (config_path.filename() == "lazyverilog.toml") {
                            root_ = config_path.parent_path();
                            config_found_ = true;
                        }
                    }
                }

                // Re-read config from disk on every configuration-change
                // notification.  Apply project-parse inputs with one analyzer
                // transaction so a single config change schedules at most one
                // full-project background reindex generation.
                std::string warn;
                ConfigWarning warning_detail;
                config_ = load_config(root_, &warn, &warning_detail);
                impl_->diag_debounce_ms = config_.lint.diagnostic_debounce_ms;
                std::cerr << "[lazyverilog] reloaded config from "
                          << (root_ / "lazyverilog.toml").string() << "\n";
                if (!warn.empty())
                    show_warning(warn);
                publish_config_diagnostic(warn.empty() ? nullptr : &warning_detail);
                { auto vcode = load_vcode(root_, config_);
                  analyzer_.set_project_config(config_.design.define, vcode.include_dirs,
                                               vcode.files, resolve_vcode_path(root_, config_)); }
                configure_background_compiler();
                schedule_background_compilation();
            } catch (const std::exception& e) {
                std::cerr << "[lazyverilog] didChangeConfiguration error: " << e.what() << "\n";
            }
        });

    // ── workspace/didChangeWatchedFiles ─────────────────────────────────────
    ep.registerHandler([&](const Notify_WorkspaceDidChangeWatchedFiles::notify& note) {
        try {
            std::vector<std::string> changed_uris;
            std::vector<std::string> deleted_uris;
            changed_uris.reserve(note.params.changes.size());
            deleted_uris.reserve(note.params.changes.size());

            for (const auto& change : note.params.changes) {
                const auto& uri = change.uri.raw_uri_;
                if (uri.empty())
                    continue;
                if (change.type == lsFileChangeType::Deleted)
                    deleted_uris.push_back(uri);
                else
                    changed_uris.push_back(uri);
            }

            // Event-driven project-shard refresh.  This deliberately avoids
            // workspace scans, mtime polling, and per-request stat() calls; the
            // client/watcher tells us exactly which files changed.  Closed files
            // are reparsed by the background indexer, while open files use their
            // current DocumentState when the worker reaches them.
            analyzer_.refresh_changed_extra_files(changed_uris, deleted_uris);
            for (const auto& uri : changed_uris)
                analyzer_.clear_semantic_diagnostics(uri);
            for (const auto& uri : deleted_uris)
                analyzer_.clear_semantic_diagnostics(uri);
            schedule_background_compilation();
        } catch (const std::exception& e) {
            std::cerr << "[lazyverilog] didChangeWatchedFiles error: " << e.what() << "\n";
        }
    });

    // ── textDocument/didOpen ──────────────────────────────────────────────────
    ep.registerHandler([&, show_warning](const Notify_TextDocumentDidOpen::notify& note) {
        try {
            const auto& td = note.params.textDocument;
            // Walk up from file to find lazyverilog.toml if not already found
            if (!config_found_) {
                auto uri = td.uri.raw_uri_;
                if (uri.starts_with("file://"))
                    uri = uri.substr(7);
                auto found = find_config_root(uri);
                if (!found.empty()) {
                    root_ = found;
                    config_found_ = true;
                    std::string warn;
                    ConfigWarning warning_detail;
                    config_ = load_config(root_, &warn, &warning_detail);
                impl_->diag_debounce_ms = config_.lint.diagnostic_debounce_ms;
                    if (!warn.empty())
                        show_warning(warn);
                    publish_config_diagnostic(warn.empty() ? nullptr : &warning_detail);
                    { auto vcode = load_vcode(root_, config_);
                      analyzer_.set_project_config(config_.design.define, vcode.include_dirs,
                                                   vcode.files, resolve_vcode_path(root_, config_)); }
                    configure_background_compiler();
                }
            }
            analyzer_.open(td.uri.raw_uri_, td.text);
            document_versions_[td.uri.raw_uri_] = td.version;
            analyzer_.clear_semantic_diagnostics(td.uri.raw_uri_);
            publish_diagnostics(td.uri.raw_uri_);
            schedule_background_compilation();
        } catch (const std::exception& e) {
            std::cerr << "[lazyverilog] didOpen error: " << e.what() << "\n";
        }
    });

    // ── textDocument/didChange ────────────────────────────────────────────────
    ep.registerHandler([&](const Notify_TextDocumentDidChange::notify& note) {
        try {
            const auto& uri = note.params.textDocument.uri.raw_uri_;
            if (note.params.textDocument.version) {
                const int incoming_version = *note.params.textDocument.version;
                const auto known = document_versions_.find(uri);
                if (known != document_versions_.end() && incoming_version <= known->second) {
                    // Drop stale/duplicate notifications.  For WorkspaceEdit
                    // application (rename, code action, etc.) the client is
                    // authoritative: once it sends a newer didChange below, we
                    // reparse from the reported text and refresh dependents.
                    return;
                }
                document_versions_[uri] = incoming_version;
            }
            if (!note.params.contentChanges.empty()) {
                auto state = analyzer_.get_state(uri);
                std::string text = state ? state->text : "";
                for (const auto& chg : note.params.contentChanges)
                    text = apply_incremental_change(std::move(text), chg);
                analyzer_.update_text(uri, text);
            }
            schedule_diagnostics(uri);
        } catch (const std::exception& e) {
            std::cerr << "[lazyverilog] didChange error: " << e.what() << "\n";
        }
    });

    // ── textDocument/didClose ─────────────────────────────────────────────────
    ep.registerHandler([&](const Notify_TextDocumentDidClose::notify& note) {
        try {
            const auto& uri = note.params.textDocument.uri.raw_uri_;
            analyzer_.close(uri);
            document_versions_.erase(uri);
            clear_published_diagnostics_for_owner(uri);
        } catch (const std::exception& e) {
            std::cerr << "[lazyverilog] didClose error: " << e.what() << "\n";
        }
    });

    // ── textDocument/formatting ───────────────────────────────────────────────
    ep.registerHandler([&, show_warning](const td_formatting::request& req) {
        td_formatting::response rsp;
        rsp.id = req.id;
        try {
            const auto& uri = req.params.textDocument.uri.raw_uri_;
            auto state = analyzer_.get_state(uri);
            if (state) {
                std::string text = state->text;
                FormatOptions save_format = config_.format;
                if (config_.autoarg.autoarg_on_save && state->tree) {
                    auto results = autoarg_all_modules(*state);
                    // apply back-to-front so earlier offsets stay valid
                    std::sort(results.begin(), results.end(),
                              [](const AutoargResult& a, const AutoargResult& b) {
                                  return a.open_line > b.open_line;
                              });
                    for (const auto& result : results) {
                        size_t line_start = lsp_offset(text, result.open_line, 0);
                        size_t open = lsp_offset(text, result.open_line, result.open_col);
                        std::string line_prefix = text.substr(line_start, open - line_start);
                        // Generate the AutoArg port list.  If whole-document formatting is
                        // enabled, leave the fragment unformatted here and let the single final
                        // document pass normalize both the generated ports and the surrounding
                        // source.  Formatting each fragment first is wasted work on save because
                        // the full pass immediately reformats the same text again.
                        std::string replacement =
                            line_prefix + format_autoarg(result, config_.autoarg, save_format);
                        if (!save_format.enable_format_on_save)
                            replacement = format_emit_text(replacement, save_format);
                        size_t end = lsp_offset(text, result.end_line, result.end_col);
                        text.replace(line_start, end - line_start, replacement);
                    }
                }

                if (save_format.enable_format_on_save)
                    text = format_source(text, save_format);

                if (text != state->text)
                    rsp.result.push_back(whole_document_edit(*state, text));
            }
        } catch (const SafeModeError& e) {
            show_warning(e.what());
        } catch (const std::exception& e) {
            std::cerr << "[lazyverilog] formatting error: " << e.what() << "\n";
        }
        return rsp;
    });

    // ── textDocument/rangeFormatting ──────────────────────────────────────────
    ep.registerHandler([&, show_warning](const td_rangeFormatting::request& req) {
        td_rangeFormatting::response rsp;
        rsp.id = req.id;
        try {
            const auto& uri = req.params.textDocument.uri.raw_uri_;
            auto state = analyzer_.get_state(uri);
            if (state) {
                // Format the full document so the formatter has surrounding context, but return
                // an edit restricted to the requested range.
                std::string formatted = format_source(state->text, config_.format);
                auto edit = range_format_edit(state->text, formatted, req.params.range);
                if (edit.newText != slice_lsp_range(state->text, edit.range))
                    rsp.result.push_back(std::move(edit));
            }
        } catch (const SafeModeError& e) {
            show_warning(e.what());
        } catch (const std::exception& e) {
            std::cerr << "[lazyverilog] range formatting error: " << e.what() << "\n";
        }
        return rsp;
    });

    // ── textDocument/hover ────────────────────────────────────────────────────
    ep.registerHandler([&](const td_hover::request& req) {
        td_hover::response rsp;
        rsp.id = req.id;
        try {
            if (auto hover = provide_hover(analyzer_, req.params))
                rsp.result = std::move(*hover);
        } catch (const std::exception& e) {
            std::cerr << "[lazyverilog] hover error: " << e.what() << "\n";
        }
        return rsp;
    });

    // ── textDocument/definition ───────────────────────────────────────────────
    ep.registerHandler([&](const td_definition::request& req) {
        td_definition::response rsp;
        rsp.id = req.id;
        try {
            auto loc = provide_definition(analyzer_, req.params);
            if (loc) {
                rsp.result =
                    std::make_pair(optional<std::vector<lsLocation>>(std::vector<lsLocation>{*loc}),
                                   optional<std::vector<LocationLink>>{});
            }
        } catch (const std::exception& e) {
            std::cerr << "[lazyverilog] definition error: " << e.what() << "\n";
        }
        return rsp;
    });

    // ── textDocument/references ───────────────────────────────────────────────
    ep.registerHandler([&](const td_references::request& req) {
        td_references::response rsp;
        rsp.id = req.id;
        try {
            rsp.result = provide_references(analyzer_, req.params);
        } catch (const std::exception& e) {
            std::cerr << "[lazyverilog] references error: " << e.what() << "\n";
        }
        return rsp;
    });

    // ── textDocument/prepareRename ────────────────────────────────────────────
    ep.registerHandler([&](const td_prepareRename::request& req) {
        td_prepareRename::response rsp;
        rsp.id = req.id;
        try {
            if (auto prepared = prepare_rename(analyzer_, req.params))
                rsp.result = std::make_pair(optional<lsRange>{},
                                            optional<PrepareRenameResult>(std::move(*prepared)));
        } catch (const std::exception& e) {
            std::cerr << "[lazyverilog] prepareRename error: " << e.what() << "\n";
        }
        return rsp;
    });

    // ── textDocument/rename ───────────────────────────────────────────────────
    ep.registerHandler([&](const td_rename::request& req) {
        td_rename::response rsp;
        rsp.id = req.id;
        try {
            rsp.result = provide_rename(analyzer_, req.params);
        } catch (const std::exception& e) {
            std::cerr << "[lazyverilog] rename error: " << e.what() << "\n";
        }
        return rsp;
    });

    // ── textDocument/foldingRange ─────────────────────────────────────────────
    ep.registerHandler([&](const td_foldingRange::request& req) {
        td_foldingRange::response rsp;
        rsp.id = req.id;
        try {
            rsp.result = provide_folding_range(analyzer_, req.params);
        } catch (const std::exception& e) {
            std::cerr << "[lazyverilog] foldingRange error: " << e.what() << "\n";
        }
        return rsp;
    });

    // ── textDocument/completion ───────────────────────────────────────────────
    ep.registerHandler([&](const td_completion::request& req) {
        td_completion::response rsp;
        rsp.id = req.id;
        try {
            const auto& uri = req.params.textDocument.uri.raw_uri_;
            auto state = analyzer_.get_state(uri);
            if (!state) {
                rsp.result = CompletionList{};
                return rsp;
            }
            CancellationToken tok;
            rsp.result = impl_->completion_engine.complete(req.params, *state, analyzer_, tok);
        } catch (const CompletionCancelled&) {
            rsp.result = CompletionList{};
        } catch (const std::exception& e) {
            std::cerr << "[lazyverilog] completion error: " << e.what() << "\n";
        }
        return rsp;
    });

    // ── textDocument/signatureHelp ────────────────────────────────────────────
    ep.registerHandler([&](const td_signatureHelp::request& req) {
        td_signatureHelp::response rsp;
        rsp.id = req.id;
        try {
            if (auto help = provide_signature_help(analyzer_, req.params))
                rsp.result = std::move(*help);
        } catch (const std::exception& e) {
            std::cerr << "[lazyverilog] signatureHelp error: " << e.what() << "\n";
        }
        return rsp;
    });

    // ── textDocument/inlayHint ────────────────────────────────────────────────
    ep.registerHandler([&](const td_inlayHint::request& req) {
        td_inlayHint::response rsp;
        rsp.id = req.id;
        try {
            if (!config_.inlay_hint.enable)
                return rsp;
            const auto& uri = req.params.textDocument.uri.raw_uri_;
            rsp.result = provide_inlay_hints(analyzer_, uri, req.params.range.start.line,
                                             req.params.range.end.line);
        } catch (const std::exception& e) {
            std::cerr << "[lazyverilog] inlayHint error: " << e.what() << "\n";
        }
        return rsp;
    });

    // ── workspace/symbol ──────────────────────────────────────────────────────
    ep.registerHandler([&](const wp_symbol::request& req) {
        wp_symbol::response rsp;
        rsp.id = req.id;
        try {
            rsp.result = provide_workspace_symbols(analyzer_, req.params);
        } catch (const std::exception& e) {
            std::cerr << "[lazyverilog] workspaceSymbol error: " << e.what() << "\n";
        }
        return rsp;
    });

    // ── textDocument/codeAction ───────────────────────────────────────────────
    ep.registerHandler([&, show_warning](const td_codeActionCode::request& req) {
        td_codeActionCode::response rsp;
        rsp.id = req.id;
        try {
            rsp.result = provide_code_actions(analyzer_, config_, req.params);
        } catch (const SafeModeError& e) {
            show_warning(e.what());
        } catch (const std::exception& e) {
            std::cerr << "[lazyverilog] codeAction error: " << e.what() << "\n";
        }
        return rsp;
    });

    // ── workspace/executeCommand ──────────────────────────────────────────────
    ep.registerHandler([&, show_warning](const wp_executeCommand::request& req) {
        wp_executeCommand::response rsp;
        rsp.id = req.id;
        // executeCommand result is optional in the protocol, but LspCpp serializes an unset Any as
        // invalid JSON (`"result":}`). Default to null so error/no-op paths stay parseable.
        rsp.result.SetJsonString("null", lsp::Any::kNullType);
        try {
            const auto& cmd = req.params.command;
            const auto& args = req.params.arguments;

            auto get_string = [&](size_t i) -> std::string {
                if (!args || i >= args->size())
                    return {};
                std::string s;
                auto value = (*args)[i];
                value.Get(s);
                return s;
            };
            auto get_int = [&](size_t i) -> int {
                if (!args || i >= args->size())
                    return 0;
                int v = 0;
                auto value = (*args)[i];
                value.Get(v);
                return v;
            };

            auto get_string_list = [&](size_t i) -> std::vector<std::string> {
                std::vector<std::string> out;
                std::string packed = get_string(i);
                size_t start = 0;
                while (start <= packed.size()) {
                    size_t end = packed.find('\n', start);
                    std::string item = packed.substr(start, end == std::string::npos ? std::string::npos
                                                                                      : end - start);
                    if (!item.empty())
                        out.push_back(std::move(item));
                    if (end == std::string::npos)
                        break;
                    start = end + 1;
                }
                return out;
            };

            auto lint_result_json =
                [&](const std::vector<std::shared_ptr<const DocumentState>>& states) {
                auto severity_text = [](int severity) -> std::string_view {
                    switch (severity) {
                    case 1:
                        return "Error";
                    case 2:
                        return "Warning";
                    case 4:
                        return "Hint";
                    default:
                        return "Information";
                    }
                };

                auto uri_to_file = [](std::string uri) {
                    constexpr std::string_view prefix = "file://";
                    if (uri.starts_with(prefix))
                        uri.erase(0, prefix.size());
                    return uri;
                };

                std::vector<std::pair<std::string, ParseDiagInfo>> diagnostics;
                std::unordered_set<std::string> seen_uris;

                // :LintAll is intentionally a manual, potentially slow command:
                // it receives temporary DocumentState snapshots for all .f
                // files so AST-based lint can run without retaining those ASTs
                // or mutating the persistent project index.  Cross-file rules
                // still consult the current published ProjectIndexSnapshot; the
                // command does not perform hidden reindexing as a side effect.
                std::shared_ptr<const ProjectIndexSnapshot> project_lint_index;
                if (config_.lint.module.stale_autoinst_diagnostic)
                    project_lint_index = analyzer_.project_index_snapshot();

                auto add_diag = [&](const std::string& fallback_uri, ParseDiagInfo diag) {
                    const std::string target_uri = diag.uri.empty() ? fallback_uri : diag.uri;
                    diag.uri.clear();
                    diagnostics.emplace_back(target_uri, std::move(diag));
                };

                for (const auto& state : states) {
                    if (!state)
                        continue;
                    const auto& uri = state->uri;
                    if (!seen_uris.insert(uri).second)
                        continue;

                    for (auto diag : state->parse_diagnostics)
                        add_diag(uri, std::move(diag));

                    auto lint_diags = run_lint(*state, config_.lint,
                                               project_lint_index.get());
                    for (auto diag : lint_diags)
                        add_diag(uri, std::move(diag));

                    if (config_.compilation.background_compilation) {
                        auto semantic_diags = analyzer_.semantic_diagnostics(uri);
                        for (auto diag : semantic_diags)
                            add_diag(uri, std::move(diag));
                    }
                }

                std::string json = "[";
                for (size_t i = 0; i < diagnostics.size(); ++i) {
                    if (i > 0)
                        json += ",";
                    const auto& [diag_uri, diag] = diagnostics[i];
                    json += "{";
                    json += "\"uri\":" + json_string(diag_uri);
                    json += ",\"file\":" + json_string(uri_to_file(diag_uri));
                    json += ",\"line\":" + std::to_string(diag.line + 1);
                    json += ",\"col\":" + std::to_string(diag.col + 1);
                    json += ",\"severity\":" + json_string(severity_text(diag.severity));
                    json += ",\"message\":" + json_string(diag.message);
                    json += "}";
                }
                json += "]";
                rsp.result.SetJsonString(json, lsp::Any::kArrayType);
            };

            auto preview_ff_result = [&](const AutoffResult& result) {
                // The Neovim client-side AutoFF command expects a small JSON object:
                //   { "pairs": [{ "src": "...", "dst": "...", ... }] }
                // or, for errors/warnings:
                //   { "error": "...", "warn": true }
                // Keep this separate from autoffApply, which returns a WorkspaceEdit.
                std::string json;
                json.reserve(128 + result.pairs.size() * 150);
                json += "{";
                bool need_comma = false;
                auto comma = [&]() {
                    if (need_comma)
                        json += ",";
                    need_comma = true;
                };
                if (!result.error.empty()) {
                    comma();
                    json += "\"error\":" + json_string(result.error);
                }
                if (result.warn) {
                    comma();
                    json += "\"warn\":true";
                }
                if (result.has_error) {
                    comma();
                    json += "\"has_error\":true";
                }
                comma();
                json += "\"pairs\":[";
                for (size_t i = 0; i < result.pairs.size(); ++i) {
                    const auto& pair = result.pairs[i];
                    if (i > 0)
                        json += ",";
                    json += "{";
                    json += "\"src\":" + json_string(pair.src);
                    json += ",\"dst\":" + json_string(pair.dst);
                    json += ",\"missing_if\":" + std::string(pair.missing_if ? "true" : "false");
                    json += ",\"missing_else\":" + std::string(pair.missing_else ? "true" : "false");
                    json += "}";
                }
                json += "]}";
                rsp.result.SetJsonString(json, lsp::Any::kObjectType);
            };

            auto apply_ff_edits = [&](const AutoffResult& result, const std::string& uri) {
                if (result.has_error || (result.edits.empty() && result.warn)) {
                    // Return null result
                    lsp::Any null_result;
                    null_result.SetJsonString("null", lsp::Any::kNullType);
                    rsp.result = std::move(null_result);
                    return;
                }
                if (result.edits.empty())
                    return;

                auto state = analyzer_.get_state(uri);
                if (!state)
                    return;

                // Split once and build the final text in append order.  The
                // previous implementation inserted into the middle of a vector
                // for every edit; many flip-flop declarations could therefore
                // shift the same tail lines repeatedly.
                std::vector<std::string_view> lines;
                {
                    const std::string_view tv = state->text;
                    size_t start = 0;
                    while (start <= tv.size()) {
                        size_t end = tv.find('\n', start);
                        if (end == std::string_view::npos) {
                            lines.push_back(tv.substr(start));
                            break;
                        }
                        lines.push_back(tv.substr(start, end - start));
                        start = end + 1;
                    }
                }

                std::vector<const AutoffEdit*> edits;
                edits.reserve(result.edits.size());
                for (const auto& edit : result.edits) {
                    if (edit.line >= 0 && edit.line <= static_cast<int>(lines.size()))
                        edits.push_back(&edit);
                }
                std::sort(edits.begin(), edits.end(), [](const AutoffEdit* a, const AutoffEdit* b) {
                    return a->line < b->line;
                });

                size_t next_line = 0;
                std::string new_text;
                new_text.reserve(state->text.size() + 256);
                bool emitted_line = false;
                auto append_line = [&](std::string_view line) {
                    if (emitted_line)
                        new_text += '\n';
                    new_text += line;
                    emitted_line = true;
                };
                auto append_edit_text = [&](std::string_view text) {
                    size_t pos = 0;
                    while (pos <= text.size()) {
                        const size_t nl = text.find('\n', pos);
                        if (nl == std::string_view::npos) {
                            if (pos < text.size())
                                append_line(text.substr(pos));
                            break;
                        }
                        append_line(text.substr(pos, nl - pos));
                        pos = nl + 1;
                    }
                };
                for (const auto* edit : edits) {
                    const auto edit_line = static_cast<size_t>(edit->line);
                    while (next_line < edit_line)
                        append_line(lines[next_line++]);
                    append_edit_text(edit->text);
                }
                while (next_line < lines.size())
                    append_line(lines[next_line++]);

                new_text = format_source(new_text, config_.format);

                // Build workspace edit JSON and set as result
                lsWorkspaceEdit we;
                lsTextEdit text_edit = whole_document_edit(*state, new_text);
                we.changes = std::map<std::string, std::vector<lsTextEdit>>{};
                (*we.changes)[uri] = {text_edit};

                rsp.result.SetJsonString(
                    whole_document_workspace_edit_json(uri, *state, new_text),
                    lsp::Any::kObjectType);
            };

            if (cmd == "lazyverilog.lint") {
                std::vector<std::shared_ptr<const DocumentState>> states;
                if (auto state = analyzer_.get_state(get_string(0)))
                    states.push_back(std::move(state));
                lint_result_json(states);
            } else if (cmd == "lazyverilog.lintAll") {
                lint_result_json(analyzer_.project_file_states_sync());
            } else if (cmd == "lazyverilog.format") {
                std::string uri = get_string(0);
                std::string mode = get_string(1);
                auto state = analyzer_.get_state(uri);
                if (state) {
                    std::string formatted = format_source(state->text, config_.format);
                    optional<lsTextEdit> edit;
                    if (mode == "range") {
                        int start_line = get_int(2);
                        int end_line = get_int(3);
                        lsRange range(lsPosition(start_line, 0), lsPosition(end_line, 0));
                        auto range_edit = range_format_edit(state->text, formatted, range);
                        if (range_edit.newText != slice_lsp_range(state->text, range_edit.range))
                            edit = std::move(range_edit);
                    } else if (formatted != state->text) {
                        edit = whole_document_edit(*state, formatted);
                    }
                    if (edit)
                        rsp.result.SetJsonString(workspace_edit_json(uri, *edit),
                                                 lsp::Any::kObjectType);
                }
            } else if (cmd == "lazyverilog.autoffPreview") {
                std::string uri = get_string(0);
                int ff_line = get_int(1);
                auto state = analyzer_.get_state(uri);
                if (state) {
                    auto result = preview_autoff(*state, ff_line, config_.autoff.register_pattern);
                    preview_ff_result(result);
                }
            } else if (cmd == "lazyverilog.autoffApply") {
                std::string uri = get_string(0);
                int ff_line = get_int(1);
                auto state = analyzer_.get_state(uri);
                if (state) {
                    auto result = autoff(*state, ff_line, config_.autoff.register_pattern);
                    apply_ff_edits(result, uri);
                }
            } else if (cmd == "lazyverilog.autoffAllPreview") {
                std::string uri = get_string(0);
                auto state = analyzer_.get_state(uri);
                if (state) {
                    auto result = preview_autoff_all(*state, config_.autoff.register_pattern);
                    preview_ff_result(result);
                }
            } else if (cmd == "lazyverilog.autoffAllApply") {
                std::string uri = get_string(0);
                auto state = analyzer_.get_state(uri);
                if (state) {
                    auto result = autoff_all(*state, config_.autoff.register_pattern);
                    apply_ff_edits(result, uri);
                }
            } else if (cmd == "lazyverilog.rtlTree") {
                std::string uri = get_string(0);
                if (auto tree = analyzer_.rtl_tree(uri)) {
                    rsp.result.SetJsonString(rtl_tree_json(*tree, config_.rtltree.show_file, config_.rtltree.show_instance_name), lsp::Any::kObjectType);
                }
            } else if (cmd == "lazyverilog.rtlTreeReverse") {
                std::string uri = get_string(0);
                if (auto tree = analyzer_.rtl_tree_reverse(uri)) {
                    rsp.result.SetJsonString(rtl_tree_json(*tree, config_.rtltree.show_file, config_.rtltree.show_instance_name), lsp::Any::kObjectType);
                }
            } else if (cmd == "lazyverilog.autowire" || cmd == "lazyverilog.autowirepreview") {
                std::string uri = get_string(0);
                int target_line = get_int(1);
                auto state = analyzer_.get_state(uri);
                if (state && state->tree) {
                    auto opened = analyzer_.opened_file_index_shards(uri);
                    auto project = analyzer_.project_index_snapshot();
                    std::span<const OpenIndexShard> opened_shards =
                        opened ? std::span<const OpenIndexShard>(*opened)
                               : std::span<const OpenIndexShard>{};
                    if (cmd == "lazyverilog.autowirepreview") {
                        auto preview = autowire_preview(*state, opened_shards, project.get(),
                                                        config_.autowire, target_line);
                        // Return preview lines as JSON array of strings
                        std::string json = "[";
                        for (size_t i = 0; i < preview.size(); ++i) {
                            if (i > 0)
                                json += ",";
                            json += "\"";
                            for (char c : preview[i]) {
                                if (c == '"')
                                    json += "\\\"";
                                else if (c == '\\')
                                    json += "\\\\";
                                else if (c == '\n')
                                    json += "\\n";
                                else
                                    json += c;
                            }
                            json += "\"";
                        }
                        json += "]";
                        rsp.result.SetJsonString(json, lsp::Any::kUnKnown);
                    } else {
                        std::string new_source =
                            autowire_apply(*state, opened_shards, project.get(),
                                           config_.autowire, target_line);
                        if (new_source != state->text)
                            new_source = format_source(new_source, config_.format);
                        if (new_source != state->text) {
                            lsWorkspaceEdit we;
                            lsTextEdit text_edit = whole_document_edit(*state, new_source);
                            we.changes = std::map<std::string, std::vector<lsTextEdit>>{};
                            (*we.changes)[uri] = {text_edit};
                            rsp.result.SetJsonString(
                                whole_document_workspace_edit_json(uri, *state, new_source),
                                lsp::Any::kObjectType);
                        }
                    }
                }
            } else if (cmd == "lazyverilog.connectInfo") {
                std::string uri = get_string(0);
                const bool lazy_hierarchy = get_string(1) == "lazy";
                rsp.result.SetJsonString(connect_info_json(analyzer_, uri, lazy_hierarchy),
                                         lsp::Any::kObjectType);
            } else if (cmd == "lazyverilog.connectHierarchyChildren") {
                std::string uri = get_string(0);
                std::string parent_path = get_string(1);
                rsp.result.SetJsonString(connect_hierarchy_children_json(analyzer_, uri, parent_path),
                                         lsp::Any::kObjectType);
            } else if (cmd == "lazyverilog.connectApplyPreview") {
                std::string uri = get_string(0);
                std::string source_path = get_string(1);
                std::string source_port = get_string(2);
                std::string dest_path = get_string(3);
                std::string dest_port = get_string(4);
                std::string wire_name = get_string(5);
                auto source_boundary_ports = get_string_list(6);
                auto dest_boundary_ports = get_string_list(7);
                rsp.result.SetJsonString(connect_apply_preview_json(analyzer_, uri, source_path,
                                                                     source_port, dest_path,
                                                                     dest_port, wire_name,
                                                                     source_boundary_ports,
                                                                     dest_boundary_ports),
                                         lsp::Any::kObjectType);
            } else if (cmd == "lazyverilog.connectApply") {
                std::string uri = get_string(0);
                std::string source_path = get_string(1);
                std::string source_port = get_string(2);
                std::string dest_path = get_string(3);
                std::string dest_port = get_string(4);
                std::string wire_name = get_string(5);
                auto source_boundary_ports = get_string_list(6);
                auto dest_boundary_ports = get_string_list(7);
                rsp.result.SetJsonString(connect_apply_edit_json(analyzer_, uri, source_path,
                                                                 source_port, dest_path, dest_port,
                                                                 wire_name,
                                                                 source_boundary_ports,
                                                                 dest_boundary_ports),
                                         lsp::Any::kObjectType);
            } else if (cmd == "lazyverilog.interface") {
                std::string uri = get_string(0);
                std::string inst1_name = get_string(1);
                std::string inst2_name = get_string(2);
                rsp.result.SetJsonString(interface_json(analyzer_, uri, inst1_name, inst2_name),
                                         lsp::Any::kObjectType);
            } else if (cmd == "lazyverilog.interfaceConnect") {
                std::string uri = get_string(0);
                std::string inst1_name = get_string(1);
                std::string inst2_name = get_string(2);
                std::string inst1_port = get_string(3);
                std::string inst2_port = get_string(4);
                std::string wire_name = get_string(5);
                std::string wire_type = get_string(6);
                rsp.result.SetJsonString(interface_connect_edit_json(analyzer_, uri, inst1_name,
                                                                     inst2_name, inst1_port,
                                                                     inst2_port, wire_name,
                                                                     wire_type),
                                         lsp::Any::kObjectType);
            } else if (cmd == "lazyverilog.interfaceDisconnect") {
                std::string uri = get_string(0);
                std::string inst1_name = get_string(1);
                std::string inst2_name = get_string(2);
                std::string inst1_port = get_string(3);
                std::string inst2_port = get_string(4);
                std::string signal_name = get_string(5);
                rsp.result.SetJsonString(interface_disconnect_edit_json(analyzer_, uri, inst1_name,
                                                                        inst2_name, inst1_port,
                                                                        inst2_port, signal_name),
                                         lsp::Any::kObjectType);
            } else if (cmd == "lazyverilog.singleInterface") {
                std::string uri = get_string(0);
                std::string inst_name = get_string(1);
                rsp.result.SetJsonString(single_interface_json(analyzer_, uri, inst_name),
                                         lsp::Any::kObjectType);
            } else {
                std::cerr << "[lazyverilog] unknown executeCommand: " << cmd << "\n";
            }
            if (rsp.result.GetType() == lsp::Any::Type::kUnKnown) {
                lsp::Any null_result;
                null_result.SetJsonString("null", lsp::Any::kNullType);
                rsp.result = std::move(null_result);
            }
        } catch (const SafeModeError& e) {
            show_warning(e.what());
        } catch (const std::exception& e) {
            std::cerr << "[lazyverilog] executeCommand error: " << e.what() << "\n";
        }
        return rsp;
    });
}
