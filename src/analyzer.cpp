#include "analyzer.hpp"
#include "dynamic_file_index.hpp"
#include "syntax_index_shared.hpp"
#include "string_utils.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <set>
#include <slang/diagnostics/DiagnosticEngine.h>
#include <slang/parsing/Preprocessor.h>
#include <slang/syntax/AllSyntax.h>
#include <slang/syntax/SyntaxTree.h>
#include <slang/syntax/SyntaxVisitor.h>
#include <slang/text/SourceManager.h>
#include <unordered_map>
#include <unordered_set>

namespace {

bool perf_trace_enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("LAZYVERILOG_TRACE_PERF");
        return value && *value && std::string_view(value) != "0";
    }();
    return enabled;
}

using Clock = std::chrono::steady_clock;

constexpr size_t kMaxRtlTreeDepth = 256;

void log_perf(std::string_view label, Clock::time_point start) {
    if (!perf_trace_enabled())
        return;
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start);
    std::cerr << "[lazyverilog][perf] " << label << ": " << elapsed.count() << "us\n";
}

} // namespace

static std::string file_name_to_uri(std::string_view file_name, const std::string& fallback_uri);

static int saturating_lsp_int(size_t value) {
    constexpr auto max_int = static_cast<size_t>(std::numeric_limits<int>::max());
    return value > max_int ? std::numeric_limits<int>::max() : static_cast<int>(value);
}

static size_t utf16_units_until_newline(std::string_view text, size_t pos) {
    size_t units = 0;
    while (pos < text.size() && text[pos] != '\n') {
        unsigned char c = static_cast<unsigned char>(text[pos]);
        int bytes = 1;
        int width = 1;
        if (c < 0x80) {
            bytes = 1;
        } else if ((c & 0xE0) == 0xC0) {
            bytes = 2;
        } else if ((c & 0xF0) == 0xE0) {
            bytes = 3;
        } else if ((c & 0xF8) == 0xF0) {
            bytes = 4;
            width = 2;
        }

        bool valid_sequence = pos + static_cast<size_t>(bytes) <= text.size();
        for (int i = 1; valid_sequence && i < bytes; ++i) {
            unsigned char cc = static_cast<unsigned char>(text[pos + static_cast<size_t>(i)]);
            valid_sequence = (cc & 0xC0) == 0x80;
        }
        if (!valid_sequence) {
            bytes = 1;
            width = 1;
        }

        units += static_cast<size_t>(width);
        pos += static_cast<size_t>(bytes);
    }
    return units;
}

static void cache_document_end_position(DocumentState& state) {
    size_t line = 0;
    size_t line_start = 0;
    for (size_t i = 0; i < state.text.size(); ++i) {
        if (state.text[i] == '\n') {
            ++line;
            line_start = i + 1;
        }
    }
    const size_t col = utf16_units_until_newline(state.text, line_start);
    state.end_line = saturating_lsp_int(line);
    state.end_character = saturating_lsp_int(col);
}

static void add_include_dirs(slang::SourceManager& sm, const std::vector<std::string>& dirs) {
    for (const auto& dir : dirs) {
        if (dir.empty())
            continue;
        // SourceManager reports an error only for exact non-existent directory
        // patterns.  Completion should remain best-effort when the user has a
        // stale config path, so we ignore the return code here and let missing
        // include diagnostics surface from slang in the normal parse path.
        (void)sm.addUserDirectories(dir);
    }
}

static void collect_parse_diagnostics(DocumentState& state, const std::string& fallback_uri) {
    if (!state.tree)
        return;

    // Format diagnostics immediately while the SyntaxTree arena is alive.
    // Do NOT copy slang::Diagnostic objects — their ConstantValue args can
    // contain internal pointers that are not safely copyable.
    const auto& diags = state.tree->diagnostics();
    auto& sm = state.tree->sourceManager();
    slang::DiagnosticEngine engine(sm);
    for (const auto& d : diags) {
        ParseDiagInfo info;
        try {
            auto loc = d.location.valid() ? sm.getFullyExpandedLoc(d.location) : d.location;
            info.uri = file_name_to_uri(sm.getFileName(loc), fallback_uri);
            if (loc.valid() && sm.isFileLoc(loc)) {
                size_t ln = sm.getLineNumber(loc);
                size_t col = sm.getColumnNumber(loc);
                info.line = ln > 0 ? (int)ln - 1 : 0;
                info.col = col > 0 ? (int)col - 1 : 0;
            }
        } catch (...) {
        }
        auto sev = slang::getDefaultSeverity(d.code);
        if (sev == slang::DiagnosticSeverity::Error || sev == slang::DiagnosticSeverity::Fatal)
            info.severity = 1;
        else if (sev == slang::DiagnosticSeverity::Warning)
            info.severity = 2;
        else
            info.severity = 3;
        try {
            info.message = engine.formatMessage(d);
        } catch (...) {
            info.message = "(diagnostic format error)";
        }
        state.parse_diagnostics.push_back(std::move(info));
    }
}

struct OpenTextOverlay {
    std::string uri;
    std::string path;
    // Keep the immutable open-buffer snapshot alive instead of copying its full
    // text into every overlay vector.  didChange swaps DocumentState instances,
    // so this shared_ptr gives SourceManager a stable view for the duration of
    // the parse without retaining any closed/project ASTs beyond the open-buffer
    // operation that already needed them.
    std::shared_ptr<const DocumentState> state;
};

static void preload_open_text_overlays(slang::SourceManager& sm,
                                       const std::vector<OpenTextOverlay>& overlays,
                                       const std::string& excluded_path = {}) {
    for (const auto& overlay : overlays) {
        if (overlay.path.empty() || overlay.path == excluded_path)
            continue;
        // Seed SourceManager's file cache with unsaved open-buffer text before
        // slang resolves `include directives.  This lets a dependent file such
        // as memory.sv see an unsaved rename in params.svh without polling or
        // reading every project file.  Only already-open buffers are copied.
        if (!overlay.state)
            continue;
        sm.assignText(std::string_view(overlay.path), std::string_view(overlay.state->text));
    }
}

static std::shared_ptr<DocumentState>
make_file_state_with_options(const std::filesystem::path& path,
                             const std::vector<std::string>& defines,
                             const std::vector<std::string>& include_dirs,
                             const std::vector<OpenTextOverlay>& open_overlays = {}) {
    const auto start = Clock::now();
    const auto norm = normalize_filesystem_path(path);
    const std::string uri = uri_from_path(norm);

    auto sm = std::make_unique<slang::SourceManager>();
    add_include_dirs(*sm, include_dirs);
    preload_open_text_overlays(*sm, open_overlays, norm.string());
    slang::parsing::PreprocessorOptions ppo;
    ppo.predefines = defines;
    slang::Bag bag;
    bag.set(ppo);

    auto tree_or_error = slang::syntax::SyntaxTree::fromFile(norm.string(), *sm, bag);
    if (!tree_or_error)
        return nullptr;

    auto text = read_file_text_optional(norm);
    auto state = std::make_shared<DocumentState>(uri, text.value_or(std::string{}), nullptr);
    state->normalized_path = norm.string();
    cache_document_end_position(*state);
    state->source_manager = std::move(sm);
    state->tree = std::move(*tree_or_error);
    state->include_dependencies = collect_include_dependency_uris(*state->source_manager, uri);
    state->include_dependency_set.insert(state->include_dependencies.begin(),
                                         state->include_dependencies.end());
    if (state->tree) {
        state->index = SyntaxIndex::build(*state->tree, state->text, IndexDepth::Declarations);
        state->index.include_dependencies = state->include_dependencies;
    }
    collect_parse_diagnostics(*state, uri);
    log_perf("make_file_state_with_options " + uri, start);
    return state;
}

Analyzer::~Analyzer() {
    if (background_indexer_.joinable()) {
        background_indexer_.request_stop();
        background_cv_.notify_all();
    }
}

std::shared_ptr<DocumentState> Analyzer::make_state(const std::string& uri,
                                                    const std::string& text) const {
    const auto start = Clock::now();
    // Pass URI as name (display label) and stripped filesystem path as path
    // (used by SourceManager::assignText for include resolution relative to
    // the file's directory, not the server CWD).
    std::string path = uri;
    if (path.starts_with("file://"))
        path = path.substr(7);
    // Fresh SourceManager per document snapshot: avoids "path already assigned"
    // errors when the same file is re-parsed on didChange, and prevents the
    // static singleton from accumulating stale buffers across edits.
    std::vector<std::string> defines;
    std::vector<std::string> include_dirs;
    std::vector<OpenTextOverlay> open_overlays;
    const auto normalized_current_path = normalize_filesystem_path(path).string();
    {
        std::lock_guard<std::mutex> lock(map_mutex_);
        defines = defines_;
        include_dirs = include_dirs_;
        open_overlays.reserve(docs_.size());
        for (const auto& [open_uri, open_state] : docs_) {
            if (!open_state || open_uri == uri)
                continue;
            if (open_state->normalized_path == normalized_current_path)
                continue;
            open_overlays.push_back(OpenTextOverlay{
                .uri = open_uri,
                .path = open_state->normalized_path,
                .state = open_state,
            });
        }
    }

    auto sm = std::make_unique<slang::SourceManager>();
    add_include_dirs(*sm, include_dirs);
    preload_open_text_overlays(*sm, open_overlays, normalized_current_path);
    slang::parsing::PreprocessorOptions ppo;
    ppo.predefines = defines;
    slang::Bag bag;
    bag.set(ppo);
    auto tree = slang::syntax::SyntaxTree::fromText(
        std::string_view(text), *sm, std::string_view(uri), std::string_view(path), bag);
    auto state = std::make_shared<DocumentState>(uri, text, nullptr);
    state->normalized_path = normalized_current_path;
    cache_document_end_position(*state);
    state->source_manager = std::move(sm);
    state->tree = std::move(tree);
    state->include_dependencies = collect_include_dependency_uris(*state->source_manager, uri);
    state->include_dependency_set.insert(state->include_dependencies.begin(),
                                         state->include_dependencies.end());
    // clangd-style current-file layer:
    //
    // Do not materialize any current-file SyntaxIndex on didOpen/didChange.
    // The live SyntaxTree is the current-file representation.  Features derive
    // narrow AST facts on demand, and project/background files keep the indexed
    // shard representation.
    collect_parse_diagnostics(*state, uri);
    log_perf("make_state " + uri, start);
    return state;
}

void Analyzer::open(const std::string& uri, const std::string& text) {
    auto state = make_state(uri, text);

    const auto path_string = state->normalized_path;

    bool listed_extra_file = false;
    {
        std::lock_guard<std::mutex> lock(map_mutex_);
        docs_[uri] = state;
        invalidate_extra_snapshots_locked();
        invalidate_opened_files_index_locked();
        listed_extra_file = extra_file_set_.contains(path_string);
    }

    // Building a dynamic/open-buffer SyntaxIndex may walk the full AST. Do that
    // outside map_mutex_ so a large listed file opened in the editor does not
    // block unrelated request handlers behind global analyzer state.
    if (listed_extra_file) {
        auto index = get_dynamic_index(*state);
        std::lock_guard<std::mutex> lock(map_mutex_);
        if (const auto it = docs_.find(uri); it != docs_.end() && it->second == state)
            update_extra_cache_for_live_state_locked(state, std::move(index));
    }
}

void Analyzer::change(const std::string& uri, const std::string& text) {
    auto state = make_state(uri, text);

    const auto path_string = state->normalized_path;

    bool listed_extra_file = false;
    {
        std::lock_guard<std::mutex> lock(map_mutex_);
        docs_[uri] = state;
        invalidate_extra_snapshots_locked();
        invalidate_opened_files_index_locked();
        listed_extra_file = extra_file_set_.contains(path_string);

        auto depends_on_changed_uri = [&](const DocumentState& doc) {
            return doc.include_dependency_set.contains(uri);
        };
        auto index_depends_on_changed_uri = [&](const std::vector<std::string>& deps) {
            return std::find(deps.begin(), deps.end(), uri) != deps.end();
        };

        bool queued_dependent = false;
        for (const auto& [other_uri, other_state] : docs_) {
            if (other_uri == uri || !other_state ||
                !depends_on_changed_uri(*other_state))
                continue;

            const std::string& other_path = other_state->normalized_path;
            // Indirect include fanout belongs on the background path.  A common
            // header can be included by many open files; reparsing all of them
            // synchronously on each keystroke would violate the current-file
            // AST / background-project-index split and can lag badly on shared
            // HPC filesystems.  The worker reparses live open buffers from
            // their in-memory text and open include overlays.
            background_pending_files_.push_front(other_path);
            queued_dependent = true;
        }

        for (const auto& [extra_uri, entry] : extra_cache_) {
            if (docs_.contains(extra_uri) || !index_depends_on_changed_uri(entry.index ? entry.index->include_dependencies : std::vector<std::string>{}))
                continue;
            background_pending_files_.push_front(entry.path);
            queued_dependent = true;
        }
        if (queued_dependent) {
            ++background_generation_;
            start_background_indexer_locked();
            background_cv_.notify_all();
        }
    }

    // See Analyzer::open(): current-buffer shard building is intentionally
    // outside map_mutex_ to keep the edit path responsive on large RTL files.
    if (listed_extra_file) {
        auto index = get_dynamic_index(*state);
        std::lock_guard<std::mutex> lock(map_mutex_);
        if (const auto it = docs_.find(uri); it != docs_.end() && it->second == state)
            update_extra_cache_for_live_state_locked(state, std::move(index));
    }

}

void Analyzer::close(const std::string& uri) {
    std::lock_guard<std::mutex> lock(map_mutex_);
    docs_.erase(uri);
    invalidate_extra_snapshots_locked();
    invalidate_opened_files_index_locked();
    // If the closed file is also in the filelist cache, replace its live shard
    // with a disk-backed parse in the background.  Keeping this asynchronous is
    // important for large buffers: closing a split should not synchronously
    // parse an include-heavy RTL file on the UI path.
    if (const auto it = extra_cache_.find(uri); it != extra_cache_.end()) {
        background_pending_files_.push_back(it->second.path);
        start_background_indexer_locked();
        background_cv_.notify_all();
    }
}

std::vector<std::string> Analyzer::extra_files() const {
    std::lock_guard<std::mutex> lock(map_mutex_);
    return extra_files_;
}

std::shared_ptr<const DocumentState> Analyzer::get_state(const std::string& uri) const {
    std::lock_guard<std::mutex> lock(map_mutex_);
    auto it = docs_.find(uri);
    if (it == docs_.end())
        return nullptr;
    return it->second;
}

struct IdentifierSpan {
    std::string text;
    int start_col{0};
    int end_col{0};
};

// Extract identifier at (0-based line, 0-based col) from source text.
static std::optional<IdentifierSpan> extract_ident_span(std::string_view src, int line, int col) {
    int cur = 0;
    size_t pos = 0;
    while (pos < src.size() && cur < line) {
        if (src[pos] == '\n')
            ++cur;
        ++pos;
    }
    if (cur < line)
        return std::nullopt;

    size_t ls = pos;
    size_t le = src.find('\n', pos);
    if (le == std::string_view::npos)
        le = src.size();

    if (col < 0 || (size_t)col >= le - ls)
        return std::nullopt;
    size_t ip = ls + col;

    auto is_id = [](char c) { return std::isalnum((unsigned char)c) || c == '_' || c == '$'; };
    if (!is_id(src[ip]))
        return std::nullopt;

    size_t start = ip;
    while (start > ls && is_id(src[start - 1]))
        --start;
    size_t end = ip;
    while (end < le && is_id(src[end]))
        ++end;

    return IdentifierSpan{std::string(src.substr(start, end - start)), (int)(start - ls),
                          (int)(end - ls)};
}

static bool same_location(const Location& lhs, const Location& rhs) {
    return lhs.uri == rhs.uri && lhs.line == rhs.line && lhs.col == rhs.col;
}

static std::string extract_ident(std::string_view src, int line, int col) {
    auto span = extract_ident_span(src, line, col);
    return span ? span->text : std::string{};
}

static bool is_backtick_identifier(std::string_view src, int line, int ident_start_col) {
    int cur = 0;
    size_t pos = 0;
    while (pos < src.size() && cur < line) {
        if (src[pos] == '\n')
            ++cur;
        ++pos;
    }
    if (cur < line || ident_start_col <= 0)
        return false;

    size_t line_start = pos;
    size_t line_end = src.find('\n', pos);
    if (line_end == std::string_view::npos)
        line_end = src.size();
    const size_t backtick = line_start + (size_t)ident_start_col - 1;
    return backtick < line_end && src[backtick] == '`';
}

static bool is_define_identifier(std::string_view src, int line, int ident_start_col) {
    int cur = 0;
    size_t pos = 0;
    while (pos < src.size() && cur < line) {
        if (src[pos] == '\n')
            ++cur;
        ++pos;
    }
    if (cur < line || ident_start_col <= 0)
        return false;

    const size_t line_start = pos;
    const size_t ident_start = line_start + (size_t)ident_start_col;
    if (ident_start > src.size())
        return false;

    std::string_view prefix = src.substr(line_start, ident_start - line_start);
    auto first = prefix.find_first_not_of(" \t");
    if (first == std::string_view::npos)
        return false;
    prefix.remove_prefix(first);
    return prefix.starts_with("`define") &&
           (prefix.size() == 7 || std::isspace((unsigned char)prefix[7]));
}

static int to_lsp_line(int one_based_line) { return one_based_line > 0 ? one_based_line - 1 : 0; }

static std::string file_name_to_uri(std::string_view file_name, const std::string& fallback_uri) {
    if (file_name.empty())
        return fallback_uri;

    auto uri = uri_from_file_name(file_name);
    return uri.empty() ? fallback_uri : uri;
}

static std::optional<Location>
find_module_definition(const SyntaxIndex& index, const std::string& uri, const std::string& name) {
    auto it = index.module_by_name.find(name);
    if (it == index.module_by_name.end() || it->second >= index.modules.size())
        return std::nullopt;

    const auto& module = index.modules[it->second];
    const auto actual_uri = index.source_uri(module.file_id);
    const int line = to_lsp_line(module.line);
    return Location{actual_uri.empty() ? uri : actual_uri, line, module.col, line,
                    module.col + (int)module.name.size()};
}

static const ModuleEntry* find_module_entry(const SyntaxIndex& index, const std::string& name) {
    auto it = index.module_by_name.find(name);
    if (it == index.module_by_name.end() || it->second >= index.modules.size())
        return nullptr;
    return &index.modules[it->second];
}

static const PortEntry* find_port_entry(const ModuleEntry& module, const std::string& name) {
    auto it = module.port_by_name.find(name);
    if (it == module.port_by_name.end() || it->second >= module.ports.size())
        return nullptr;
    return &module.ports[it->second];
}

static Location location_from_token_actual_uri(const slang::SourceManager& sm,
                                               const std::string& fallback_uri,
                                               const slang::parsing::Token& token);

static std::optional<Location> find_port_definition(const SyntaxIndex& index,
                                                    const std::string& uri,
                                                    const std::string& module_name,
                                                    const std::string& port_name) {
    const auto* module = find_module_entry(index, module_name);
    if (!module)
        return std::nullopt;
    const auto* port = find_port_entry(*module, port_name);
    if (!port)
        return std::nullopt;

    const auto actual_uri = index.source_uri(port->file_id);
    const int line = to_lsp_line(port->line);
    return Location{actual_uri.empty() ? uri : actual_uri, line, port->col, line,
                    port->col + (int)port->name.size()};
}

static std::optional<std::string> symbol_id_for_index_location(const SyntaxIndex& index,
                                                               const Location& loc,
                                                               bool allow_name_fallback = false) {
    auto acceptable = [&](const ReferenceEntry& ref) {
        return ref.symbol_id && (allow_name_fallback || !ref.symbol_debug.starts_with("name:"));
    };

    auto best_from_bucket = [&](const ReferenceLocationKey& key, size_t& best_index) {
        const auto bucket = index.references_by_location.find(key);
        if (bucket == index.references_by_location.end())
            return;
        for (const size_t ref_index : bucket->second) {
            if (ref_index >= index.references.size())
                continue;
            const auto& ref = index.references[ref_index];
            if (acceptable(ref) && ref_index < best_index)
                best_index = ref_index;
        }
    };

    // ReferenceEntry stores locations as shard-local FileIDs plus 1-based lines.
    // Convert the requested LSP location to the same compact key and probe both
    // the exact file bucket and the legacy invalid-file bucket.  Invalid FileID
    // entries historically meant "owning shard URI" and matched by line/column
    // only, so keeping that bucket preserves behavior for synthesized/older
    // occurrences without scanning the entire reference vector.
    const int reference_line = loc.line + 1;
    size_t best_index = index.references.size();
    if (const auto file_it = index.source_file_ids.find(loc.uri); file_it != index.source_file_ids.end()) {
        best_from_bucket(ReferenceLocationKey{
            .file_id = file_it->second,
            .line = reference_line,
            .col = loc.col,
        }, best_index);
    }
    best_from_bucket(ReferenceLocationKey{
        .file_id = kInvalidSourceFileID,
        .line = reference_line,
        .col = loc.col,
    }, best_index);

    if (best_index < index.references.size())
        return index.references[best_index].symbol_debug;
    return std::nullopt;
}

static std::optional<Location>
find_module_definition_in_tree(const slang::syntax::SyntaxTree& tree, const std::string& uri,
                               const std::string& name) {
    struct Visitor : public slang::syntax::SyntaxVisitor<Visitor> {
        const slang::SourceManager& sm;
        const std::string& uri;
        const std::string& name;
        std::optional<Location> result;

        Visitor(const slang::SourceManager& sm, const std::string& uri, const std::string& name)
            : sm(sm), uri(uri), name(name) {}

        void handle(const slang::syntax::ModuleDeclarationSyntax& node) {
            if (result)
                return;
            if (node.header->name.valueText() == name)
                result = location_from_token_actual_uri(sm, uri, node.header->name);
            if (!result)
                visitDefault(node);
        }
    };

    Visitor visitor(tree.sourceManager(), uri, name);
    tree.root().visit(visitor);
    return visitor.result;
}

static std::optional<Location>
find_port_definition_in_tree(const slang::syntax::SyntaxTree& tree, const std::string& uri,
                             const std::string& module_name, const std::string& port_name) {
    struct Visitor : public slang::syntax::SyntaxVisitor<Visitor> {
        const slang::SourceManager& sm;
        const std::string& uri;
        const std::string& module_name;
        const std::string& port_name;
        bool in_target_module{false};
        std::optional<Location> result;

        Visitor(const slang::SourceManager& sm, const std::string& uri,
                const std::string& module_name, const std::string& port_name)
            : sm(sm), uri(uri), module_name(module_name), port_name(port_name) {}

        void maybe_set(const slang::parsing::Token& token) {
            if (!result && token && token.valueText() == port_name)
                result = location_from_token_actual_uri(sm, uri, token);
        }

        void handle(const slang::syntax::ModuleDeclarationSyntax& node) {
            if (result)
                return;
            const bool was = in_target_module;
            in_target_module = node.header->name.valueText() == module_name;
            if (in_target_module)
                visitDefault(node);
            in_target_module = was;
        }

        void handle(const slang::syntax::ImplicitAnsiPortSyntax& node) {
            if (in_target_module && node.declarator)
                maybe_set(node.declarator->name);
            if (!result)
                visitDefault(node);
        }

        void handle(const slang::syntax::ExplicitAnsiPortSyntax& node) {
            if (in_target_module)
                maybe_set(node.name);
            if (!result)
                visitDefault(node);
        }

        void handle(const slang::syntax::PortDeclarationSyntax& node) {
            if (!in_target_module)
                return;
            for (const auto* declarator : node.declarators) {
                if (declarator)
                    maybe_set(declarator->name);
                if (result)
                    return;
            }
            visitDefault(node);
        }
    };

    Visitor visitor(tree.sourceManager(), uri, module_name, port_name);
    tree.root().visit(visitor);
    return visitor.result;
}

static Location location_from_token(const slang::SourceManager& sm, const std::string& uri,
                                    const slang::parsing::Token& token) {
    const int line = to_lsp_line((int)sm.getLineNumber(token.location()));
    const int col = (int)sm.getColumnNumber(token.location()) - 1;
    return Location{uri, line, col, line, col + (int)token.valueText().size()};
}

static Location location_from_token_actual_uri(const slang::SourceManager& sm,
                                               const std::string& fallback_uri,
                                               const slang::parsing::Token& token) {
    auto loc = location_from_token(
        sm, file_name_to_uri(sm.getFileName(token.location()), fallback_uri), token);
    return loc;
}

static bool macro_has_user_source_location(const slang::SourceManager& sm,
                                           const slang::parsing::Token& name) {
    // slang built-in macros are represented with SourceLocation::NoLocation.
    // Returning an LSP definition for those tokens forces us to invent a file
    // and line, which is why `SV_COV_ERROR previously jumped to line 1 of the
    // current buffer.  Treat no-location macros as invisible to user-facing
    // LSP features.
    return name && name.location().valid() && sm.isFileLoc(name.location());
}

static std::string render_hover_dimensions(
    const slang::SourceManager& sm,
    const slang::syntax::SyntaxList<slang::syntax::VariableDimensionSyntax>& dimensions) {
    std::string text;
    for (const auto* dimension : dimensions) {
        if (!dimension)
            continue;

        const auto rendered = render_syntax_node_text(sm, *dimension);
        if (rendered.empty())
            continue;

        if (syntax_needs_space_between_fragments(text, rendered))
            text += ' ';
        text += rendered;
    }
    return text;
}

static std::string append_hover_suffix(std::string base, const std::string& suffix) {
    if (suffix.empty())
        return base;
    base += (base.empty() ? "" : " ") + suffix;
    return base;
}

static std::string format_ports_doc(const std::string& ports_text) {
    auto text = trim_copy(ports_text);
    if (text.size() < 2 || text.front() != '(' || text.back() != ')')
        return text;

    auto inner = trim_copy(text.substr(1, text.size() - 2));
    if (inner.empty())
        return "()";

    std::vector<std::string> ports;
    size_t start = 0;
    while (start <= inner.size()) {
        const size_t comma = inner.find(',', start);
        if (comma == std::string::npos) {
            ports.push_back(trim_copy(inner.substr(start)));
            break;
        }
        ports.push_back(trim_copy(inner.substr(start, comma - start)));
        start = comma + 1;
    }
    if (ports.size() <= 1)
        return "(" + inner + ")";

    std::string out = "(\n";
    for (size_t i = 0; i < ports.size(); ++i) {
        out += "    " + ports[i];
        if (i + 1 != ports.size())
            out += ",\n";
    }
    out += "\n)";
    return out;
}

static std::string module_doc_from_entry(const ModuleEntry& module) {
    if (module.ports.empty())
        return {};

    size_t max_dir = 0;
    size_t max_type = 0;
    for (const auto& port : module.ports) {
        max_dir = std::max(max_dir, port.direction.size());
        max_type = std::max(max_type, port.type.size());
    }

    std::string doc = "```\nmodule " + module.name;
    for (const auto& port : module.ports) {
        doc += "\n  " + port.direction;
        doc += std::string(max_dir - port.direction.size(), ' ');
        doc += "  " + port.type;
        doc += std::string(max_type - port.type.size(), ' ');
        doc += "  " + port.name;
    }
    doc += "\n```";
    return doc;
}

static std::optional<Location> find_macro_definition(const slang::syntax::SyntaxTree& tree,
                                                     const std::string& uri,
                                                     const std::string& name) {
    const auto& sm = tree.sourceManager();

    for (const auto* macro : tree.getDefinedMacros()) {
        if (!macro || macro->name.valueText() != name)
            continue;
        if (!macro_has_user_source_location(sm, macro->name))
            continue;
        return location_from_token_actual_uri(sm, uri, macro->name);
    }
    return std::nullopt;
}

static std::optional<SymbolInfo> find_macro_info(const slang::syntax::SyntaxTree& tree,
                                                 const std::string& uri, const std::string& name) {
    const auto& sm = tree.sourceManager();

    for (const auto* macro : tree.getDefinedMacros()) {
        if (!macro || macro->name.valueText() != name)
            continue;
        if (!macro_has_user_source_location(sm, macro->name))
            continue;

        std::string body;
        for (const auto& token : macro->body)
            body += token.toString();
        body = trim_copy(body);

        auto loc = location_from_token_actual_uri(sm, uri, macro->name);
        return SymbolInfo{.name = name,
                          .kind = "macro",
                          .detail = body.empty() ? "(empty)" : body,
                          .line = loc.line,
                          .col = loc.col};
    }
    return std::nullopt;
}

static std::optional<Location>
find_subroutine_argument_definition(const slang::syntax::SyntaxTree& tree, const std::string& uri,
                                    const std::string& subroutine_name,
                                    const std::string& argument_name) {
    struct Visitor : public slang::syntax::SyntaxVisitor<Visitor> {
        const slang::SourceManager& sm;
        const std::string& uri;
        const std::string& subroutine_name;
        const std::string& argument_name;
        std::optional<Location> result;

        Visitor(const slang::SourceManager& sm, const std::string& uri,
                const std::string& subroutine_name, const std::string& argument_name)
            : sm(sm), uri(uri), subroutine_name(subroutine_name), argument_name(argument_name) {}

        void handle(const slang::syntax::FunctionDeclarationSyntax& node) {
            const auto* identifier =
                node.prototype->name->as_if<slang::syntax::IdentifierNameSyntax>();
            if (!identifier || identifier->identifier.valueText() != subroutine_name)
                return;

            if (!node.prototype->portList)
                return;

            for (const auto* port_base : node.prototype->portList->ports) {
                const auto* port =
                    port_base ? port_base->as_if<slang::syntax::FunctionPortSyntax>() : nullptr;
                if (!port)
                    continue;
                const auto& token = port->declarator->name;
                if (token.valueText() == argument_name) {
                    result = location_from_token_actual_uri(sm, uri, token);
                    return;
                }
            }
        }
    };

    Visitor visitor(tree.sourceManager(), uri, subroutine_name, argument_name);
    tree.root().visit(visitor);
    return visitor.result;
}

struct GenericDefinitionVisitor : public slang::syntax::SyntaxVisitor<GenericDefinitionVisitor> {
    const slang::SourceManager& sm;
    const std::string& uri;
    const std::string& name;
    const std::string& preferred_module;
    std::string current_module;
    std::optional<Location> first_result;
    std::optional<Location> scoped_result;

    GenericDefinitionVisitor(const slang::SourceManager& sm, const std::string& uri,
                             const std::string& name, const std::string& preferred_module)
        : sm(sm), uri(uri), name(name), preferred_module(preferred_module) {}

    std::optional<Location> result() const { return scoped_result ? scoped_result : first_result; }

    void maybe_set(slang::parsing::Token token, bool scope_sensitive = true) {
        if (!token || token.valueText() != name)
            return;

        auto loc = location_from_token_actual_uri(sm, uri, token);
        if (!first_result)
            first_result = loc;
        if (scope_sensitive && !preferred_module.empty() && current_module == preferred_module &&
            !scoped_result)
            scoped_result = loc;
    }

    void handle(const slang::syntax::ModuleDeclarationSyntax& node) {
        maybe_set(node.header->name, false);

        auto previous_module = current_module;
        current_module = std::string(node.header->name.valueText());
        visitDefault(node);
        current_module = std::move(previous_module);
    }

    void handle(const slang::syntax::ClassDeclarationSyntax& node) {
        // Class names are ordinary type identifiers at use sites:
        //
        //     packet_cfg cfg;
        //     ^^^^^^^^^^ should jump to: class packet_cfg;
        //
        // The token fallback in definition_target_at() can identify the use
        // token, but the definition search still needs to expose the class
        // declaration name as a generic definition candidate.
        maybe_set(node.name, false);
        visitDefault(node);
    }

    void handle(const slang::syntax::ImplicitAnsiPortSyntax& node) {
        maybe_set(node.declarator->name);
    }

    void handle(const slang::syntax::ExplicitAnsiPortSyntax& node) { maybe_set(node.name); }

    void handle(const slang::syntax::DeclaratorSyntax& node) { maybe_set(node.name); }

    void handle(const slang::syntax::FunctionDeclarationSyntax& node) {
        if (const auto* identifier =
                node.prototype->name->as_if<slang::syntax::IdentifierNameSyntax>())
            maybe_set(identifier->identifier);
        visitDefault(node);
    }

    void handle(const slang::syntax::TypedefDeclarationSyntax& node) {
        maybe_set(node.name);
        visitDefault(node);
    }
};

static std::optional<Location> find_generic_definition(const slang::syntax::SyntaxTree& tree,
                                                       const std::string& uri,
                                                       const std::string& name,
                                                       const std::string& preferred_module) {
    GenericDefinitionVisitor visitor(tree.sourceManager(), uri, name, preferred_module);
    tree.root().visit(visitor);
    return visitor.result();
}

static bool token_at_location(const slang::SourceManager& sm, const slang::parsing::Token& token,
                              const Location& location) {
    if (!token || !token.location().valid())
        return false;
    const auto token_location = location_from_token_actual_uri(sm, location.uri, token);
    return token_location.uri == location.uri && token_location.line == location.line &&
           token_location.col == location.col;
}

static std::string token_text(const slang::parsing::Token& token) {
    return token ? std::string(token.valueText()) : std::string{};
}

static std::string name_text(const slang::syntax::NameSyntax& name) {
    if (const auto* identifier = name.as_if<slang::syntax::IdentifierNameSyntax>())
        return token_text(identifier->identifier);
    return trim_copy(name.toString());
}

static std::optional<SymbolInfo>
symbol_info_from_definition(const slang::syntax::SyntaxTree& tree, const std::string& uri,
                            const std::string& name, const Location& definition,
                            const SyntaxIndex* prebuilt_index = nullptr) {
    struct Visitor : public slang::syntax::SyntaxVisitor<Visitor> {
        const slang::SourceManager& sm;
        const std::string& uri;
        const std::string& name;
        const Location& definition;
        const SyntaxIndex& index;
        std::optional<SymbolInfo> result;

        Visitor(const slang::SourceManager& sm, const std::string& uri, const std::string& name,
                const Location& definition, const SyntaxIndex& index)
            : sm(sm), uri(uri), name(name), definition(definition), index(index) {}

        void set_from_token(const slang::parsing::Token& token, std::string kind,
                            std::string detail) {
            if (result || token.valueText() != name || !token_at_location(sm, token, definition))
                return;
            result = SymbolInfo{.name = name,
                                .kind = std::move(kind),
                                .detail = std::move(detail),
                                .line = definition.line,
                                .col = definition.col};
        }

        void handle(const slang::syntax::ModuleDeclarationSyntax& node) {
            if (result || node.header->name.valueText() != name ||
                !token_at_location(sm, node.header->name, definition)) {
                visitDefault(node);
                return;
            }

            std::string doc;
            for (const auto& module : index.modules) {
                if (module.name == name) {
                    doc = module_doc_from_entry(module);
                    break;
                }
            }

            result = SymbolInfo{.name = name,
                                .kind = "module",
                                .detail = "module",
                                .doc = std::move(doc),
                                .line = definition.line,
                                .col = definition.col};
        }

        void handle(const slang::syntax::ClassDeclarationSyntax& node) {
            if (result || node.name.valueText() != name ||
                !token_at_location(sm, node.name, definition)) {
                visitDefault(node);
                return;
            }

            result = SymbolInfo{.name = name,
                                .kind = "class",
                                .detail = "class",
                                .line = definition.line,
                                .col = definition.col};
        }

        void handle(const slang::syntax::ImplicitAnsiPortSyntax& node) {
            if (!node.declarator)
                return;
            std::string detail;
            if (node.header) {
                if (const auto* variable =
                        node.header->as_if<slang::syntax::VariablePortHeaderSyntax>()) {
                    detail = token_text(variable->direction);
                    auto type = render_syntax_node_text(sm, *variable->dataType);
                    if (!type.empty())
                        detail += (detail.empty() ? "" : " ") + type;
                } else if (const auto* net =
                               node.header->as_if<slang::syntax::NetPortHeaderSyntax>()) {
                    detail = token_text(net->direction);
                    auto type = render_syntax_node_text(sm, *net->dataType);
                    if (!type.empty())
                        detail += (detail.empty() ? "" : " ") + type;
                }
            }

            // ANSI ports have the same split-type shape as ordinary
            // declarations: the header owns the direction and shared packed
            // type, while the declarator owns any unpacked dimensions.
            //
            //     module m(input logic [1:0] i_data [7:0]);
            //              ^^^^^^^^^^^^^^^^^ shared header type
            //                                      ^^^^^ declarator dimension
            //
            // Hover should show the full object type, not just the header type.
            detail = append_hover_suffix(detail,
                                         render_hover_dimensions(sm, node.declarator->dimensions));
            set_from_token(node.declarator->name, "port", detail);
        }

        void handle(const slang::syntax::ExplicitAnsiPortSyntax& node) {
            set_from_token(node.name, "port", token_text(node.direction));
        }

        void handle(const slang::syntax::PortDeclarationSyntax& node) {
            std::string detail;
            if (const auto* variable =
                    node.header->as_if<slang::syntax::VariablePortHeaderSyntax>()) {
                detail = token_text(variable->direction);
                auto type = render_syntax_node_text(sm, *variable->dataType);
                if (!type.empty())
                    detail += (detail.empty() ? "" : " ") + type;
            } else if (const auto* net = node.header->as_if<slang::syntax::NetPortHeaderSyntax>()) {
                detail = token_text(net->direction);
                auto type = render_syntax_node_text(sm, *net->dataType);
                if (!type.empty())
                    detail += (detail.empty() ? "" : " ") + type;
            }
            for (const auto* declarator : node.declarators) {
                if (!declarator)
                    continue;

                // Non-ANSI port declarations can also place unpacked
                // dimensions on individual declarators:
                //
                //     input logic [1:0] a [7:0], b [3:0];
                //
                // The header detail is shared, so copy it before appending each
                // declarator's dimensions.
                auto port_detail = detail;
                port_detail =
                    append_hover_suffix(port_detail,
                                        render_hover_dimensions(sm, declarator->dimensions));
                set_from_token(declarator->name, "port", port_detail);
            }
        }

        void handle(const slang::syntax::DataDeclarationSyntax& node) {
            // A data declaration's syntactic data type is shared by every
            // declarator in the declaration:
            //
            //     logic [7:0] a, b [4];
            //     ^^^^^^^^^^^ shared DataDeclarationSyntax::type
            //
            // SystemVerilog also allows each declarator to carry unpacked
            // dimensions.  Those dimensions are part of the declared object's
            // full type even though they are syntactically attached to the
            // declarator instead of the shared data type.  Hover should
            // therefore show:
            //
            //     a -> logic [7:0]
            //     b -> logic [7:0] [4]
            //
            // rather than the previous empty detail string, which made a
            // variable hover display only "**name** — *variable*".
            auto base_type = render_syntax_node_text(sm, *node.type);
            for (const auto* declarator : node.declarators) {
                if (!declarator)
                    continue;

                auto detail = base_type;
                detail = append_hover_suffix(detail,
                                             render_hover_dimensions(sm, declarator->dimensions));
                set_from_token(declarator->name, "variable", detail);
            }
        }

        void handle(const slang::syntax::NetDeclarationSyntax& node) {
            auto detail = token_text(node.netType);
            auto type = render_syntax_node_text(sm, *node.type);
            if (!type.empty())
                detail += (detail.empty() ? "" : " ") + type;
            for (const auto* declarator : node.declarators) {
                if (!declarator)
                    continue;

                auto net_detail = detail;
                net_detail = append_hover_suffix(net_detail,
                                                 render_hover_dimensions(sm, declarator->dimensions));
                set_from_token(declarator->name, "net", net_detail);
            }
        }

        void handle(const slang::syntax::StructUnionMemberSyntax& node) {
            // Struct / union fields are not DataDeclarationSyntax nodes.  They
            // have their own StructUnionMemberSyntax shape:
            //
            //     typedef struct {
            //         logic [7:0] addr;
            //         logic       valid;
            //     } packet_t;
            //
            // Generic definition lookup can correctly land on the field's
            // DeclaratorSyntax, but hover must still reconstruct the field's
            // declaration type from StructUnionMemberSyntax::type plus any
            // declarator-local unpacked dimensions.
            auto base_type = render_syntax_node_text(sm, *node.type);
            for (const auto* declarator : node.declarators) {
                if (!declarator)
                    continue;

                auto detail = base_type;
                detail = append_hover_suffix(detail,
                                             render_hover_dimensions(sm, declarator->dimensions));
                set_from_token(declarator->name, "variable", detail);
            }
        }

        void handle(const slang::syntax::LocalVariableDeclarationSyntax& node) {
            // Local declarations use a different syntax node from module/class
            // data declarations, but the hover policy is the same: show the
            // variable kind plus the declared data type.  Keep this in the
            // definition-to-symbol layer instead of the hover renderer so hover
            // formatting remains a pure presentation step over SymbolInfo.
            auto base_type = render_syntax_node_text(sm, *node.type);
            for (const auto* declarator : node.declarators) {
                if (!declarator)
                    continue;

                auto detail = base_type;
                detail = append_hover_suffix(detail,
                                             render_hover_dimensions(sm, declarator->dimensions));
                set_from_token(declarator->name, "variable", detail);
            }
        }

        void handle(const slang::syntax::ParameterDeclarationSyntax& node) {
            auto detail = render_syntax_node_text(sm, *node.type);
            for (const auto* declarator : node.declarators) {
                if (!declarator)
                    continue;
                auto param_detail = detail;
                if (declarator->initializer)
                    param_detail += (param_detail.empty() ? "" : " ") + std::string("= ") +
                                    trim_copy(declarator->initializer->expr->toString());
                set_from_token(declarator->name, "parameter", param_detail);
            }
        }

        void handle(const slang::syntax::FunctionPortSyntax& node) {
            auto detail = token_text(node.direction);
            if (node.dataType) {
                auto type = render_syntax_node_text(sm, *node.dataType);
                if (!type.empty())
                    detail += (detail.empty() ? "" : " ") + type;
            }
            detail = append_hover_suffix(detail,
                                         render_hover_dimensions(sm, node.declarator->dimensions));
            set_from_token(node.declarator->name, "argument", detail);
        }

        void handle(const slang::syntax::FunctionDeclarationSyntax& node) {
            if (const auto* identifier =
                    node.prototype->name->as_if<slang::syntax::IdentifierNameSyntax>()) {
                auto kind =
                    node.kind == slang::syntax::SyntaxKind::TaskDeclaration ? "task" : "function";
                std::string doc;
                std::string detail(kind);
                if (node.kind == slang::syntax::SyntaxKind::TaskDeclaration) {
                    auto ports =
                        node.prototype->portList ? node.prototype->portList->toString() : "";
                    doc = "```\ntask " + name + format_ports_doc(ports) + "\n```";
                } else {
                    auto return_type = render_syntax_node_text(sm, *node.prototype->returnType);
                    auto ports =
                        node.prototype->portList ? node.prototype->portList->toString() : "";
                    doc = "```\nfunction " + return_type + " " + name + format_ports_doc(ports) +
                          "\n```";
                }
                if (identifier->identifier.valueText() == name &&
                    token_at_location(sm, identifier->identifier, definition)) {
                    result = SymbolInfo{.name = name,
                                        .kind = kind,
                                        .detail = detail,
                                        .doc = std::move(doc),
                                        .line = definition.line,
                                        .col = definition.col};
                }
            }
            if (!result)
                visitDefault(node);
        }

        void handle(const slang::syntax::TypedefDeclarationSyntax& node) {
            set_from_token(node.name, "typedef", render_syntax_node_text(sm, *node.type));
            if (!result)
                visitDefault(node);
        }
    };

    // Hover/symbol info is a request-local AST query.  Older code built a full
    // SyntaxIndex here when the caller did not provide one, solely to enrich a
    // module hover document.  That recreated the exact anti-pattern we are
    // removing: a point query should not silently index the whole current file.
    SyntaxIndex empty_index;
    const auto& index = prebuilt_index ? *prebuilt_index : empty_index;
    Visitor visitor(tree.sourceManager(), uri, name, definition, index);
    tree.root().visit(visitor);
    return visitor.result;
}

static slang::SourceRange visible_range_for_token(const slang::SourceManager& sm,
                                                  const slang::parsing::Token& token) {
    if (sm.isMacroLoc(token.location()))
        return sm.getExpansionRange(token.location());
    return token.range();
}

static bool contains_position(const slang::SourceManager& sm, slang::SourceRange range, int line,
                              int col) {
    if (!range.start().valid() || !range.end().valid())
        return false;

    const int start_line = to_lsp_line((int)sm.getLineNumber(range.start()));
    const int start_col = (int)sm.getColumnNumber(range.start()) - 1;
    const int end_line = to_lsp_line((int)sm.getLineNumber(range.end()));
    const int end_col = (int)sm.getColumnNumber(range.end()) - 1;

    if (line < start_line || line > end_line)
        return false;
    if (line == start_line && col < start_col)
        return false;
    if (line == end_line && col >= end_col)
        return false;
    return true;
}

static bool range_starts_in_uri(const slang::SourceManager& sm, slang::SourceRange range,
                                const std::string& uri) {
    if (!range.start().valid())
        return false;
    return file_name_to_uri(sm.getFileName(range.start()), uri) == uri;
}

static bool contains_position_in_uri(const slang::SourceManager& sm, slang::SourceRange range,
                                     const std::string& uri, int line, int col) {
    return range_starts_in_uri(sm, range, uri) && contains_position(sm, range, line, col);
}

static bool token_contains_position_in_uri(const slang::SourceManager& sm,
                                           const slang::parsing::Token& token,
                                           const std::string& uri, int line, int col) {
    return token && token.location().valid() &&
           contains_position_in_uri(sm, visible_range_for_token(sm, token), uri, line, col);
}

enum class DefinitionTargetKind {
    None,
    Instance,
    NamedPort,
    NamedParameter,
    NamedArgument,
    Macro,
    Generic,
};

struct DefinitionTarget {
    DefinitionTargetKind kind{DefinitionTargetKind::None};
    std::string name;
    std::string module_name;
    std::string subroutine_name;
    std::string scope_module;
};

struct DefinitionTargetVisitor : public slang::syntax::SyntaxVisitor<DefinitionTargetVisitor> {
    const slang::SourceManager& sm;
    const std::string& uri;
    int line;
    int col;
    DefinitionTarget target;
    std::string current_module;

    DefinitionTargetVisitor(const slang::SourceManager& sm, const std::string& uri, int line,
                            int col)
        : sm(sm), uri(uri), line(line), col(col) {}

    bool found() const { return target.kind != DefinitionTargetKind::None; }

    void handle(const slang::syntax::ModuleDeclarationSyntax& node) {
        if (token_contains_position_in_uri(sm, node.header->name, uri, line, col)) {
            target.kind = DefinitionTargetKind::Generic;
            target.name = std::string(node.header->name.valueText());
            target.scope_module = current_module;
            return;
        }

        auto previous_module = current_module;
        current_module = std::string(node.header->name.valueText());
        visitDefault(node);
        current_module = std::move(previous_module);
    }

    void handle(const slang::syntax::HierarchyInstantiationSyntax& node) {
        if (token_contains_position_in_uri(sm, node.type, uri, line, col)) {
            target.kind = DefinitionTargetKind::Instance;
            target.name = std::string(node.type.valueText());
            target.module_name = target.name;
            target.scope_module = current_module;
            return;
        }

        const std::string module_name(node.type.valueText());
        if (node.parameters) {
            for (const auto* parameter : node.parameters->parameters) {
                const auto* named =
                    parameter ? parameter->as_if<slang::syntax::NamedParamAssignmentSyntax>()
                              : nullptr;
                if (!named)
                    continue;
                if (token_contains_position_in_uri(sm, named->name, uri, line, col)) {
                    target.kind = DefinitionTargetKind::NamedParameter;
                    target.name = std::string(named->name.valueText());
                    target.module_name = module_name;
                    target.scope_module = current_module;
                    return;
                }
            }
        }

        for (const auto* instance : node.instances) {
            if (!instance)
                continue;
            if (instance->decl &&
                token_contains_position_in_uri(sm, instance->decl->name, uri, line, col)) {
                target.kind = DefinitionTargetKind::Instance;
                target.name = std::string(instance->decl->name.valueText());
                target.module_name = module_name;
                target.scope_module = current_module;
                return;
            }

            for (const auto* connection : instance->connections) {
                if (!connection)
                    continue;
                const auto* named = connection->as_if<slang::syntax::NamedPortConnectionSyntax>();
                if (named && token_contains_position_in_uri(sm, named->name, uri, line, col)) {
                    target.kind = DefinitionTargetKind::NamedPort;
                    target.name = std::string(named->name.valueText());
                    target.module_name = module_name;
                    target.scope_module = current_module;
                    return;
                }
            }
        }
        visitDefault(node);
    }

    void handle(const slang::syntax::InvocationExpressionSyntax& node) {
        const auto* callee = node.left->as_if<slang::syntax::IdentifierNameSyntax>();
        if (!callee || !node.arguments) {
            visitDefault(node);
            return;
        }

        const std::string subroutine_name(callee->identifier.valueText());
        for (const auto* argument : node.arguments->parameters) {
            if (!argument)
                continue;
            const auto* named = argument->as_if<slang::syntax::NamedArgumentSyntax>();
            if (!named)
                continue;
            if (token_contains_position_in_uri(sm, named->name, uri, line, col)) {
                target.kind = DefinitionTargetKind::NamedArgument;
                target.name = std::string(named->name.valueText());
                target.subroutine_name = subroutine_name;
                target.scope_module = current_module;
                return;
            }
        }

        visitDefault(node);
    }

    void handle(const slang::syntax::NamedTypeSyntax& node) {
        const auto* identifier = node.name->as_if<slang::syntax::IdentifierNameSyntax>();
        if (identifier &&
            token_contains_position_in_uri(sm, identifier->identifier, uri, line, col)) {
            target.kind = DefinitionTargetKind::Generic;
            target.name = std::string(identifier->identifier.valueText());
            target.scope_module = current_module;
            return;
        }
        visitDefault(node);
    }

    void visitToken(slang::parsing::Token token) {
        if (found() || !token || !token.location().valid())
            return;

        if (sm.isMacroLoc(token.location())) {
            if (!contains_position_in_uri(sm, sm.getExpansionRange(token.location()), uri, line,
                                          col))
                return;

            auto macro_name = sm.getMacroName(token.location());
            if (!macro_name.empty()) {
                target.kind = DefinitionTargetKind::Macro;
                target.name = std::string(macro_name);
                target.scope_module = current_module;
            }
            return;
        }

        if (token.kind == slang::parsing::TokenKind::Identifier &&
            token_contains_position_in_uri(sm, token, uri, line, col)) {
            target.kind = DefinitionTargetKind::Generic;
            target.name = std::string(token.valueText());
            target.scope_module = current_module;
        }
    }
};

static DefinitionTarget definition_target_at(const slang::syntax::SyntaxTree& tree,
                                             const std::string& uri, int line, int col) {
    DefinitionTargetVisitor visitor(tree.sourceManager(), uri, line, col);
    tree.root().visit(visitor);
    return visitor.target;
}

std::optional<SymbolInfo> Analyzer::symbol_at(const std::string& uri, int line, int col) const {
    auto state = get_state(uri);
    if (!state || !state->tree)
        return std::nullopt;

    auto ident = extract_ident(state->text, line, col);
    if (ident.empty())
        return std::nullopt;

    auto target = definition_target_at(*state->tree, uri, line, col);
    auto extra_files = extra_file_snapshot_ptr();

    if (target.kind == DefinitionTargetKind::Macro) {
        if (auto info = find_macro_info(*state->tree, uri, target.name))
            return info;
        for (const auto& extra : *extra_files) {
            if (extra.uri == uri)
                continue;
            if (!extra.state || !extra.state->tree)
                continue;
            if (auto info = find_macro_info(*extra.state->tree, extra.uri, target.name))
                return info;
        }
        return SymbolInfo{
            .name = target.name, .kind = "macro", .detail = "(empty)", .line = line, .col = col};
    }

    // Reuse the extra-file snapshot already collected for hover.  Calling
    // definition_of() here would collect the same snapshot again, and for open
    // filelist entries that can mean repeating live-buffer index work during a
    // single user-visible hover request.
    auto definition = definition_of_state(*state, uri, line, col, *extra_files, &uri);
    if (definition) {
        std::string name = target.name.empty() ? ident : target.name;
        if (definition->uri == uri) {
            if (auto info = symbol_info_from_definition(*state->tree, uri, name, *definition))
                return info;
        } else {
            for (const auto& extra : *extra_files) {
                if (extra.uri != definition->uri || !extra.state || !extra.state->tree)
                    continue;
                if (auto info = symbol_info_from_definition(*extra.state->tree, extra.uri, name,
                                                            *definition, &extra.index))
                    return info;
            }
        }

        if (target.kind == DefinitionTargetKind::Instance)
            return SymbolInfo{.name = target.module_name,
                              .kind = "module",
                              .detail = "module",
                              .line = definition->line,
                              .col = definition->col};
        if (target.kind == DefinitionTargetKind::NamedArgument)
            return SymbolInfo{.name = target.name,
                              .kind = "argument",
                              .line = definition->line,
                              .col = definition->col};
        if (target.kind == DefinitionTargetKind::NamedPort)
            return SymbolInfo{.name = target.name,
                              .kind = "port",
                              .line = definition->line,
                              .col = definition->col};
        if (target.kind == DefinitionTargetKind::NamedParameter)
            return SymbolInfo{.name = target.name,
                              .kind = "parameter",
                              .line = definition->line,
                              .col = definition->col};
        return SymbolInfo{
            .name = name, .kind = "symbol", .line = definition->line, .col = definition->col};
    }

    return SymbolInfo{.name = ident, .kind = "unknown", .line = line, .col = col};
}

std::optional<IdentifierAtPosition> Analyzer::identifier_at(const std::string& uri, int line,
                                                            int col) const {
    auto state = get_state(uri);
    if (!state || !state->tree)
        return std::nullopt;

    struct Visitor : public slang::syntax::SyntaxVisitor<Visitor> {
        const slang::SourceManager& sm;
        const std::string& uri;
        int line;
        int col;
        std::optional<IdentifierAtPosition> result;

        Visitor(const slang::SourceManager& sm, const std::string& uri, int line, int col)
            : sm(sm), uri(uri), line(line), col(col) {}

        void visitToken(slang::parsing::Token token) {
            if (result || !token || token.kind != slang::parsing::TokenKind::Identifier)
                return;
            if (!token_contains_position_in_uri(sm, token, uri, line, col))
                return;

            const int token_line = to_lsp_line((int)sm.getLineNumber(token.location()));
            const int token_col = (int)sm.getColumnNumber(token.location()) - 1;
            const std::string name(token.valueText());
            result = IdentifierAtPosition{
                .name = name,
                .line = token_line,
                .col = token_col,
                .end_col = token_col + (int)name.size(),
            };
        }
    };

    Visitor visitor(state->tree->sourceManager(), uri, line, col);
    state->tree->root().visit(visitor);
    return visitor.result;
}

std::optional<Location> Analyzer::definition_of(const std::string& uri, int line, int col) const {
    const auto start = Clock::now();
    auto state = get_state(uri);
    if (!state || !state->tree)
        return std::nullopt;

    auto extra = extra_file_snapshot_ptr();
    // Skip the current document during extra-file iteration to avoid searching
    // it twice.  The snapshot itself is shared and immutable, so this remains
    // O(1) instead of copying and erase/removing a potentially large filelist
    // vector on every goto-definition request.
    auto result = definition_of_state(*state, uri, line, col, *extra, &uri);
    log_perf("definition_of " + uri + ":" + std::to_string(line) + ":" + std::to_string(col),
             start);
    return result;
}

std::optional<Location>
Analyzer::definition_of_state(const DocumentState& state, const std::string& uri, int line, int col,
                              std::span<const ExtraFileInfo> extra_files,
                              const std::string* skip_extra_uri) const {
    if (!state.tree)
        return std::nullopt;

    auto skip_extra = [&](const ExtraFileInfo& extra) {
        return skip_extra_uri && extra.uri == *skip_extra_uri;
    };

    auto target = definition_target_at(*state.tree, uri, line, col);

    if (target.kind == DefinitionTargetKind::None || target.name.empty()) {
        auto ident = extract_ident_span(state.text, line, col);
        if (!ident || (!is_backtick_identifier(state.text, line, ident->start_col) &&
                       !is_define_identifier(state.text, line, ident->start_col)))
            return std::nullopt;

        if (auto loc = find_macro_definition(*state.tree, uri, ident->text))
            return loc;
        for (const auto& extra : extra_files) {
            if (skip_extra(extra))
                continue;
            if (!extra.state || !extra.state->tree)
                continue;
            if (auto loc = find_macro_definition(*extra.state->tree, extra.uri, ident->text))
                return loc;
        }
        return std::nullopt;
    }

    if (target.kind == DefinitionTargetKind::Macro) {
        if (auto loc = find_macro_definition(*state.tree, uri, target.name))
            return loc;
        for (const auto& extra : extra_files) {
            if (skip_extra(extra))
                continue;
            if (!extra.state || !extra.state->tree)
                continue;
            if (auto loc = find_macro_definition(*extra.state->tree, extra.uri, target.name))
                return loc;
        }
        return std::nullopt;
    }

    if (target.kind == DefinitionTargetKind::NamedPort) {
        if (auto loc = find_port_definition_in_tree(*state.tree, uri, target.module_name,
                                                    target.name))
            return loc;

        for (const auto& extra : extra_files) {
            if (skip_extra(extra))
                continue;
            if (auto loc =
                    find_port_definition(extra.index, extra.uri, target.module_name, target.name))
                return loc;
        }
        return std::nullopt;
    }

    if (target.kind == DefinitionTargetKind::NamedParameter) {
        if (auto loc = find_port_definition_in_tree(*state.tree, uri, target.module_name,
                                                    target.name))
            return loc;

        for (const auto& extra : extra_files) {
            if (skip_extra(extra))
                continue;
            if (auto loc =
                    find_port_definition(extra.index, extra.uri, target.module_name, target.name))
                return loc;
        }
        return std::nullopt;
    }

    if (target.kind == DefinitionTargetKind::NamedArgument) {
        if (auto loc = find_subroutine_argument_definition(*state.tree, uri, target.subroutine_name,
                                                           target.name))
            return loc;

        for (const auto& extra : extra_files) {
            if (skip_extra(extra))
                continue;
            if (!extra.state || !extra.state->tree)
                continue;
            if (auto loc = find_subroutine_argument_definition(*extra.state->tree, extra.uri,
                                                               target.subroutine_name, target.name))
                return loc;
        }
        return std::nullopt;
    }

    if (target.kind == DefinitionTargetKind::Instance) {
        if (auto loc = find_module_definition_in_tree(*state.tree, uri, target.module_name))
            return loc;
        for (const auto& extra : extra_files) {
            if (skip_extra(extra))
                continue;
            if (auto loc = find_module_definition(extra.index, extra.uri, target.module_name))
                return loc;
        }
        return std::nullopt;
    }

    if (auto loc = find_generic_definition(*state.tree, uri, target.name, target.scope_module))
        return loc;
    for (const auto& extra : extra_files) {
        if (skip_extra(extra))
            continue;
        if (!extra.state || !extra.state->tree)
            continue;
        if (auto loc = find_generic_definition(*extra.state->tree, extra.uri, target.name,
                                               target.scope_module))
            return loc;
    }

    return std::nullopt;
}

std::vector<Location> Analyzer::find_references(const std::string& uri, int line, int col,
                                                bool include_declaration) const {
    auto target = identifier_at(uri, line, col);
    // References/Rename must not walk closed project-file ASTs.  Current and
    // other open files are live SyntaxTrees; closed project files require a
    // future reference-occurrence index before they can participate scalably.
    std::vector<ExtraFileInfo> extra_files;
    auto state = get_state(uri);
    if (!state)
        return {};

    // Keep one immutable view of the closed-file project index for the whole
    // request.  Grabbing extra_index_snapshot_ptr() repeatedly would both
    // contend on map_mutex_ and could mix different background-index
    // generations in one references response if a didChange / shard publish
    // happened between loops.
    const auto extra_idx = extra_index_snapshot_ptr();
    if (!target) {
        // Macro invocations are often represented in slang's parsed tree as
        // expansion tokens rather than as an ordinary Identifier token at the
        // user's source location.  `definition_of_state()` already has a raw
        // source fallback for backtick identifiers; references need the same
        // seed identifier so "find references" works when the cursor is on
        // `FOO in source text.
        if (auto ident = extract_ident_span(state->text, line, col);
            ident && (is_backtick_identifier(state->text, line, ident->start_col) ||
                      is_define_identifier(state->text, line, ident->start_col))) {
            target = IdentifierAtPosition{.name = ident->text,
                                          .line = line,
                                          .col = ident->start_col,
                                          .end_col = ident->end_col};
        }
    }
    if (!target)
        return {};
    const auto target_info = state->tree ? definition_target_at(*state->tree, uri, line, col)
                                         : DefinitionTarget{};
    auto target_def = definition_of_state(*state, uri, line, col, extra_files);
    if (!target_def && target_info.kind == DefinitionTargetKind::Instance) {
        for (const auto& extra : *extra_idx) {
            if ((target_def = find_module_definition(extra.index, extra.uri,
                                                     target_info.module_name)))
                break;
        }
    } else if (!target_def && target_info.kind == DefinitionTargetKind::NamedPort) {
        for (const auto& extra : *extra_idx) {
            if ((target_def = find_port_definition(extra.index, extra.uri,
                                                   target_info.module_name, target_info.name)))
                break;
        }
    } else if (!target_def && target_info.kind == DefinitionTargetKind::NamedParameter) {
        for (const auto& extra : *extra_idx) {
            if ((target_def = find_port_definition(extra.index, extra.uri,
                                                   target_info.module_name, target_info.name)))
                break;
        }
    }
    if (!target_def)
        return {};

    std::string target_symbol_debug;
    if (target_info.kind == DefinitionTargetKind::Instance) {
        // Go-to-definition on an instance name resolves to the instantiated
        // module declaration.  Use the module symbol identity for the closed
        // project occurrence index, matching other instantiations of that
        // module and the declaration itself instead of all same-spelled
        // instance names.
        target_symbol_debug = "module::" + target_info.module_name;
    } else if (target_info.kind == DefinitionTargetKind::NamedPort) {
        target_symbol_debug = "module_port::" + target_info.module_name + "::" + target_info.name;
    } else if (target_info.kind == DefinitionTargetKind::NamedParameter) {
        target_symbol_debug =
            "module_param::" + target_info.module_name + "::" + target_info.name;
    } else if (target_info.kind == DefinitionTargetKind::Macro) {
        target_symbol_debug = "macro::" + target_info.name;
    } else if (is_backtick_identifier(state->text, line, target->col) ||
               is_define_identifier(state->text, line, target->col)) {
        // Raw backtick fallback: if the cursor did not map to an expansion
        // token (or the cursor is on a `define name), still use the same macro
        // SymbolID that the syntax index emits for declarations and invocation
        // sites.
        target_symbol_debug = "macro::" + target->name;
    }
    if (target_symbol_debug.empty()) {
        // Prefer the symbol identity at the token the user actually clicked.
        // This matters for declaration tokens whose plain name appears in
        // multiple declaration scopes:
        //
        //   typedef struct { logic addr; } a_t;
        //   typedef struct { logic addr; } b_t;
        //                          ^ clicked here
        //
        // A generic definition fallback may find the first `addr` declaration
        // textually, but the current-file structural index has a scoped
        // declaration occurrence at the clicked location:
        //
        //   typedef_field::b_t::addr
        //
        // Recovering that ID first prevents same-name typedef fields from being
        // merged by references/rename.
        auto current_structural_index = get_structural_index(*state);
        Location clicked_loc{uri, target->line, target->col, target->line, target->end_col};
        if (auto id = symbol_id_for_index_location(current_structural_index, clicked_loc))
            target_symbol_debug = *id;

        // If the cursor was on a declaration or on a syntactic generic token
        // such as a hierarchy type, recover the project-index SymbolID from the
        // declaration location.  This keeps "find references from declaration"
        // precise without keeping closed-file ASTs alive.
        if (target_symbol_debug.empty() && target_def->uri == uri) {
            if (auto id = symbol_id_for_index_location(current_structural_index, *target_def))
                target_symbol_debug = *id;
        }
        if (target_symbol_debug.empty()) {
            for (const auto& extra : *extra_idx) {
                if (extra.uri != target_def->uri)
                    continue;
                if (auto id = symbol_id_for_index_location(extra.index, *target_def))
                    target_symbol_debug = *id;
                break;
            }
        }
    }
    std::string fallback_symbol_debug;
    if (target_symbol_debug.empty()) {
        // If the lightweight index cannot prove a scope-qualified identity, keep
        // open-buffer references on the AST/definition-verification path below
        // to avoid same-name false positives.  Closed project files cannot be
        // walked as ASTs, so retain a conservative unresolved-name SymbolID for
        // their compact occurrence shards.  This fixes the "empty references"
        // case for symbols whose only indexed identity is `name:<identifier>`
        // without downgrading precise open-file reference searches.
        auto current_structural_index = get_structural_index(*state);
        if (target_def->uri == uri) {
            if (auto id = symbol_id_for_index_location(current_structural_index, *target_def, true);
                id && id->starts_with("name:")) {
                // Do not use unresolved `name:<identifier>` as a bridge from an
                // open buffer into closed project shards.  The open-buffer AST
                // path below can verify same-definition references precisely,
                // but a closed shard cannot distinguish scopes for generic
                // names such as a module-local function `calc` versus an
                // unrelated global `calc`.  Owner-qualified SymbolIDs above
                // still enable scalable cross-file references for modules,
                // ports, parameters, typedef fields, macros, etc.
                fallback_symbol_debug.clear();
            }
        }
        if (fallback_symbol_debug.empty()) {
            for (const auto& extra : *extra_idx) {
                if (extra.uri != target_def->uri)
                    continue;
                if (auto id = symbol_id_for_index_location(extra.index, *target_def, true);
                    id && id->starts_with("name:")) {
                    fallback_symbol_debug = *id;
                }
                break;
            }
        }
    }
    const SymbolID target_symbol_id = SymbolID::from_canonical(target_symbol_debug);
    const SymbolID fallback_symbol_id = SymbolID::from_canonical(fallback_symbol_debug);

    std::vector<Location> result;
    std::set<std::tuple<std::string, int, int>> seen;

    auto add_if_same_definition =
        [&](const std::string& file_uri, int ref_line, int ref_col,
            const std::function<std::optional<Location>(const std::string&, int, int)>& resolver) {
            auto candidate_def = resolver(file_uri, ref_line, ref_col);
            if (!candidate_def || !same_location(*candidate_def, *target_def))
                return;
            if (!include_declaration && file_uri == target_def->uri &&
                ref_line == target_def->line && ref_col == target_def->col)
                return;

            auto key = std::make_tuple(file_uri, ref_line, ref_col);
            if (!seen.insert(key).second)
                return;
            result.push_back(Location{file_uri, ref_line, ref_col, ref_line,
                                      ref_col + (int)target->name.size()});
        };

    auto visit_tree =
        [&](const slang::syntax::SyntaxTree& tree, const std::string& fallback_uri,
            const std::function<std::optional<Location>(const std::string&, int, int)>& resolver) {
            struct Visitor : public slang::syntax::SyntaxVisitor<Visitor> {
                const slang::SourceManager& sm;
                const std::string& fallback_uri;
                const std::string& name;
                const std::function<void(const std::string&, int, int)>& add;

                Visitor(const slang::SourceManager& sm, const std::string& fallback_uri,
                        const std::string& name,
                        const std::function<void(const std::string&, int, int)>& add)
                    : sm(sm), fallback_uri(fallback_uri), name(name), add(add) {}

                void visitToken(slang::parsing::Token token) {
                    if (!token || token.kind != slang::parsing::TokenKind::Identifier ||
                        token.valueText() != name)
                        return;

                    const auto loc = location_from_token_actual_uri(sm, fallback_uri, token);
                    add(loc.uri, loc.line, loc.col);
                }
            };

            std::function<void(const std::string&, int, int)> add =
                [&](const std::string& candidate_uri, int ref_line, int ref_col) {
                    add_if_same_definition(candidate_uri, ref_line, ref_col, resolver);
                };

            Visitor visitor(tree.sourceManager(), fallback_uri, target->name, add);
            tree.root().visit(visitor);
        };

    std::vector<std::pair<std::string, std::shared_ptr<const DocumentState>>> open_states;
    {
        std::lock_guard<std::mutex> lock(map_mutex_);
        open_states.reserve(docs_.size());
        for (const auto& [state_uri, state] : docs_)
            open_states.emplace_back(state_uri, state);
    }

    std::unordered_map<std::string, std::shared_ptr<const DocumentState>> open_state_by_uri;
    std::unordered_set<std::string> open_uris;
    for (const auto& [state_uri, state] : open_states) {
        open_state_by_uri[state_uri] = state;
        open_uris.insert(state_uri);
    }

    auto resolve_snapshot = [&](const std::string& candidate_uri, int ref_line,
                                int ref_col) -> std::optional<Location> {
        if (auto it = open_state_by_uri.find(candidate_uri); it != open_state_by_uri.end()) {
            if (!it->second)
                return std::nullopt;
            return definition_of_state(*it->second, candidate_uri, ref_line, ref_col, extra_files);
        }
        return std::nullopt;
    };

    auto add_indexed_reference = [&](const std::string& file_uri, const SyntaxIndex& index,
                                     const ReferenceEntry& ref) {
        // `file_uri` is the parsed shard / open document URI.  Reference
        // occurrences can originate from an included header inside that shard,
        // so prefer the per-token URI when the index captured one:
        //
        //   memory.sv     `include "params.svh"
        //   params.svh    task add_number; endtask
        //
        // Without this, the params.svh line/column would be reported against
        // memory.sv and could appear to point at an unrelated token such as an
        // input port named `address`.
        const auto actual_uri = index.source_uri(ref.file_id);
        const std::string& result_uri = actual_uri.empty() ? file_uri : actual_uri;
        const int ref_line = to_lsp_line(ref.line);
        if (!include_declaration && result_uri == target_def->uri &&
            ref_line == target_def->line && ref.col == target_def->col)
            return;

        auto key = std::make_tuple(result_uri, ref_line, ref.col);
        if (!seen.insert(key).second)
            return;
        result.push_back(Location{
            .uri = result_uri,
            .line = ref_line,
            .col = ref.col,
            .end_line = ref_line,
            .end_col = ref.end_col,
        });
    };

    for (const auto& [state_uri, state] : open_states) {
        if (!state || !state->tree)
            continue;

        if (target_symbol_id) {
            // For owner-qualified symbols (module / port / parameter), use the
            // same compact occurrence representation for open files that closed
            // project files use.  This is important for cross-file open buffers:
            //
            //   memory.sv      module memory; endmodule
            //   memory_top.sv  memory u_mem();
            //
            // Resolving `memory` in memory_top.sv through definition_of_state()
            // would require closed/project-file ASTs in the resolver.  The
            // SymbolID path avoids that by matching `module:memory` directly.
            const auto open_index = get_structural_index(*state);
            for (const auto& ref : open_index.references) {
                if (ref.symbol_id == target_symbol_id ||
                    (fallback_symbol_id && ref.symbol_id == fallback_symbol_id))
                    add_indexed_reference(state_uri, open_index, ref);
            }
        } else {
            visit_tree(*state->tree, state_uri, resolve_snapshot);
        }
    }

    // Closed project files are represented by compact reference-occurrence
    // shards.  We intentionally do not load or walk their full SyntaxTrees here.
    for (const auto& extra : *extra_idx) {
        if (open_uris.contains(extra.uri))
            continue;
        if (!target_symbol_id && !fallback_symbol_id)
            continue;
        for (const auto& ref : extra.index.references) {
            if (target_symbol_id) {
                if (ref.symbol_id != target_symbol_id &&
                    (!fallback_symbol_id || ref.symbol_id != fallback_symbol_id))
                    continue;
            } else if (ref.symbol_id != fallback_symbol_id) {
                continue;
            }

            add_indexed_reference(extra.uri, extra.index, ref);
        }
    }

    std::sort(result.begin(), result.end(), [](const Location& a, const Location& b) {
        return std::tie(a.uri, a.line, a.col) < std::tie(b.uri, b.line, b.col);
    });
    return result;
}

std::vector<std::pair<int, int>> Analyzer::find_occurrences(const std::string& uri,
                                                            const std::string& name) const {
    auto state = get_state(uri);
    if (!state || name.empty())
        return {};

    std::string_view src = state->text;
    auto is_id = [](char c) { return std::isalnum((unsigned char)c) || c == '_' || c == '$'; };

    // Build line-start offsets
    std::vector<size_t> ls;
    ls.push_back(0);
    for (size_t i = 0; i < src.size(); ++i)
        if (src[i] == '\n')
            ls.push_back(i + 1);

    std::vector<std::pair<int, int>> result;
    size_t pos = 0;
    while (pos < src.size()) {
        auto found = src.find(name, pos);
        if (found == std::string_view::npos)
            break;

        bool before_ok = (found == 0) || !is_id(src[found - 1]);
        bool after_ok = (found + name.size() >= src.size()) || !is_id(src[found + name.size()]);

        if (before_ok && after_ok) {
            auto it = std::upper_bound(ls.begin(), ls.end(), found);
            int line = (int)(it - ls.begin()) - 1; // 0-based
            int col = (int)(found - ls[(size_t)line]);
            result.push_back({line, col});
        }
        pos = found + 1;
    }
    return result;
}

void Analyzer::set_project_index_publish_debounce_ms(int debounce_ms) {
    std::lock_guard<std::mutex> lock(map_mutex_);
    background_publish_debounce_ms_ = std::max(0, debounce_ms);
}

void Analyzer::set_defines(const std::vector<std::string>& defines) {
    std::lock_guard<std::mutex> lock(map_mutex_);
    defines_ = defines;
    // Invalidate extra-file cache so reopened files pick up the new defines.
    extra_cache_.clear();
    invalidate_extra_snapshots_locked();
    clear_project_index_snapshot_locked();
    if (!extra_files_.empty())
        schedule_background_reindex_locked();
}

void Analyzer::set_include_dirs(const std::vector<std::string>& include_dirs) {
    std::vector<std::string> normalized_include_dirs;
    normalized_include_dirs.reserve(include_dirs.size());
    for (const auto& dir : include_dirs)
        normalized_include_dirs.push_back(normalize_filesystem_path(dir).string());

    std::lock_guard<std::mutex> lock(map_mutex_);
    include_dirs_ = std::move(normalized_include_dirs);

    // Include paths affect parsing every explicit filelist source.  Clear the
    // cache even if the filelist itself did not change, otherwise a newly added
    // UVM include directory would not be visible until the next source edit.
    extra_cache_.clear();
    invalidate_extra_snapshots_locked();
    clear_project_index_snapshot_locked();
    if (!extra_files_.empty())
        schedule_background_reindex_locked();
}

void Analyzer::set_extra_files(const std::vector<std::string>& paths,
                               const std::string& filelist_path) {
    std::vector<std::string> normalized_paths;
    normalized_paths.reserve(paths.size());
    for (const auto& path : paths)
        normalized_paths.push_back(normalize_filesystem_path(path).string());

    std::lock_guard<std::mutex> lock(map_mutex_);
    filelist_path_ = filelist_path;
    extra_files_ = std::move(normalized_paths);
    extra_file_set_.clear();
    extra_file_set_.reserve(extra_files_.size());
    for (const auto& path : extra_files_)
        extra_file_set_.insert(path);
    extra_cache_.clear();
    invalidate_extra_snapshots_locked();
    clear_project_index_snapshot_locked();

    // Always index configured project files asynchronously, regardless of
    // whether they came from a .f file or an explicit path list.  This keeps
    // configuration reload / startup from synchronously parsing large designs.
    if (!extra_files_.empty())
        schedule_background_reindex_locked();
}

void Analyzer::set_project_config(const std::vector<std::string>& defines,
                                  const std::vector<std::string>& include_dirs,
                                  const std::vector<std::string>& extra_files,
                                  const std::string& filelist_path) {
    std::vector<std::string> normalized_include_dirs;
    normalized_include_dirs.reserve(include_dirs.size());
    for (const auto& dir : include_dirs)
        normalized_include_dirs.push_back(normalize_filesystem_path(dir).string());

    std::vector<std::string> normalized_extra_files;
    normalized_extra_files.reserve(extra_files.size());
    for (const auto& path : extra_files)
        normalized_extra_files.push_back(normalize_filesystem_path(path).string());

    std::lock_guard<std::mutex> lock(map_mutex_);

    // Apply every parse-affecting project input under one lock.  A config reload
    // commonly changes several of these at once (for example a new .f file plus
    // +incdir+ entries and preprocessor defines).  Clearing and scheduling once
    // avoids creating redundant background generations that cannot commit but
    // can still burn CPU / shared-filesystem bandwidth while they parse.
    defines_ = defines;
    include_dirs_ = std::move(normalized_include_dirs);

    filelist_path_ = filelist_path;
    extra_files_ = std::move(normalized_extra_files);
    extra_file_set_.clear();
    extra_file_set_.reserve(extra_files_.size());
    for (const auto& path : extra_files_)
        extra_file_set_.insert(path);

    extra_cache_.clear();
    invalidate_extra_snapshots_locked();
    clear_project_index_snapshot_locked();

    if (!extra_files_.empty())
        schedule_background_reindex_locked();
}

void Analyzer::refresh_changed_extra_files(const std::vector<std::string>& changed_uris,
                                           const std::vector<std::string>& deleted_uris) {
    auto normalized_project_path = [](std::string uri) -> std::string {
        if (uri.starts_with("file://"))
            uri = uri.substr(7);
        if (uri.empty())
            return {};
        return normalize_filesystem_path(uri).string();
    };

    std::vector<std::string> deleted_paths;
    deleted_paths.reserve(deleted_uris.size());
    for (const auto& uri : deleted_uris)
        deleted_paths.push_back(normalized_project_path(uri));

    std::vector<std::string> changed_paths;
    changed_paths.reserve(changed_uris.size());
    for (const auto& uri : changed_uris)
        changed_paths.push_back(normalized_project_path(uri));

    std::lock_guard<std::mutex> lock(map_mutex_);

    bool queued_changed_file = false;
    bool removed_deleted_file = false;
    std::unordered_set<std::string> seen_changed_paths;

    for (const auto& path : deleted_paths) {
        if (path.empty() || !extra_file_set_.contains(path))
            continue;
        extra_cache_.erase(uri_from_path(path));
        invalidate_extra_snapshots_locked();
        removed_deleted_file = true;
    }

    for (const auto& path : changed_paths) {
        if (path.empty() || !extra_file_set_.contains(path) || !seen_changed_paths.insert(path).second)
            continue;

        // Queue only the explicitly reported file.  This is the important HPC
        // property: a rename/workspace-edit notification does not trigger a
        // whole-design rescan and does not perform metadata checks for every
        // filelist entry.  The background worker will parse disk contents for a
        // closed file, or use the live DocumentState if the file is open.
        background_pending_files_.push_front(path);
        queued_changed_file = true;
    }

    if (!queued_changed_file && !removed_deleted_file)
        return;

    // Invalidate any in-flight parse that started before the client reported
    // these edits.  Pending unrelated files remain queued and will parse under
    // the new generation when the worker reaches them.
    ++background_generation_;

    if (removed_deleted_file)
        schedule_background_project_publish_locked();
    if (queued_changed_file) {
        start_background_indexer_locked();
        background_cv_.notify_all();
    }
}

void Analyzer::wait_for_background_index_idle() const {
    std::unique_lock<std::mutex> lock(map_mutex_);
    background_cv_.wait(lock, [&] {
        return background_pending_files_.empty() && !background_index_active_ &&
               !background_publish_requested_;
    });
}

void Analyzer::invalidate_extra_snapshots_locked() const {
    // Both snapshot vectors summarize extra_cache_.  The ExtraFileInfo variant
    // also records which project files are currently open by consulting docs_,
    // so document open/change/close paths must invalidate these caches too.
    // This helper is intentionally tiny and must only be called while
    // map_mutex_ is held by the mutating path.
    extra_file_snapshot_cache_.reset();
    extra_index_snapshot_cache_.reset();
}

void Analyzer::invalidate_opened_files_index_locked() const {
    // Open-buffer indexes are keyed by the current URI because that URI must be
    // excluded from the merge: the active request derives current-file facts
    // directly from its live AST.  Any didOpen/didChange/didClose replaces the
    // open-buffer layer, so all per-current-URI variants become stale together.
    opened_files_index_cache_.clear();
    ++opened_files_index_generation_;
}

std::shared_ptr<const std::vector<ExtraFileInfo>>
Analyzer::build_extra_file_snapshot_locked() const {
    auto result = std::make_shared<std::vector<ExtraFileInfo>>();
    result->reserve(extra_cache_.size());
    for (const auto& [key, entry] : extra_cache_) {
        // Closed project files intentionally have no DocumentState here.  They
        // are represented only by the compact SyntaxIndex shard.  If the file
        // is open, attach the live state so AST-only features can inspect
        // unsaved text without keeping closed project ASTs alive.
        //
        // Do not rebuild the open file's dynamic index here.  Open filelist
        // entries replace their shard in update_extra_cache_for_live_state_locked()
        // on didOpen/didChange, so entry.index is already the live-buffer shard.
        // Rebuilding while map_mutex_ is held would make hover/definition/RTL
        // requests serialize behind AST-derived indexing work.
        if (const auto it = docs_.find(entry.uri); it != docs_.end() && it->second) {
            result->push_back(ExtraFileInfo{
                .path = entry.path,
                .uri = entry.uri,
                .state = it->second,
                .index = entry.index ? *entry.index : SyntaxIndex{},
            });
            continue;
        }
        result->push_back(ExtraFileInfo{
            .path = entry.path,
            .uri = entry.uri,
            .state = nullptr,
            .index = entry.index ? *entry.index : SyntaxIndex{},
        });
    }
    return result;
}

std::shared_ptr<const std::vector<ExtraIndexInfo>>
Analyzer::build_extra_index_snapshot_locked() const {
    auto result = std::make_shared<std::vector<ExtraIndexInfo>>();
    result->reserve(extra_cache_.size());
    for (const auto& [key, entry] : extra_cache_) {
        result->push_back(ExtraIndexInfo{
            .path = entry.path,
            .uri = entry.uri,
            .index = entry.index ? *entry.index : SyntaxIndex{},
        });
    }
    return result;
}

std::shared_ptr<const std::vector<ExtraFileInfo>>
Analyzer::extra_file_snapshot_ptr() const {
    const auto start = Clock::now();
    std::lock_guard<std::mutex> lock(map_mutex_);
    // Do not poll the .f file mtime on request paths.  The filelist is treated
    // as configuration loaded at startup / config reload.  This avoids metadata
    // I/O in HPC environments and keeps edits to listed open buffers
    // incremental through update_extra_cache_for_live_state_locked().
    // Request paths must remain read-only with respect to parsing.  If the
    // cache is still warming or some configured files failed to parse, return
    // the shards currently available instead of synchronously parsing missing
    // files under map_mutex_.
    if (!extra_file_snapshot_cache_)
        extra_file_snapshot_cache_ = build_extra_file_snapshot_locked();
    log_perf("extra_file_snapshot_ptr files=" + std::to_string(extra_file_snapshot_cache_->size()), start);
    return extra_file_snapshot_cache_;
}

std::shared_ptr<const std::vector<ExtraIndexInfo>>
Analyzer::extra_index_snapshot_ptr() const {
    const auto start = Clock::now();
    std::lock_guard<std::mutex> lock(map_mutex_);
    if (!extra_index_snapshot_cache_)
        extra_index_snapshot_cache_ = build_extra_index_snapshot_locked();
    log_perf("extra_index_snapshot_ptr files=" + std::to_string(extra_index_snapshot_cache_->size()), start);
    return extra_index_snapshot_cache_;
}

std::shared_ptr<const ProjectIndexSnapshot> Analyzer::project_index_snapshot() const {
    const auto start = Clock::now();
    std::lock_guard<std::mutex> lock(map_mutex_);

    if (!project_index_snapshot_cache_)
        project_index_snapshot_cache_ = std::make_shared<ProjectIndexSnapshot>();
    log_perf("project_index_snapshot shards=" + std::to_string(project_index_snapshot_cache_->shards.size()), start);
    return project_index_snapshot_cache_;
}

void Analyzer::set_project_index_publish_callback(std::function<void()> callback) {
    std::lock_guard<std::mutex> lock(map_mutex_);
    project_index_publish_callback_ = std::move(callback);
}

std::shared_ptr<const SyntaxIndex>
Analyzer::opened_files_index(const std::string& current_uri) const {
    for (;;) {
        std::vector<std::shared_ptr<const DocumentState>> states;
        uint64_t generation = 0;
        {
            std::lock_guard<std::mutex> lock(map_mutex_);
            if (auto cached = opened_files_index_cache_.find(current_uri);
                cached != opened_files_index_cache_.end())
                return cached->second;

            generation = opened_files_index_generation_;
            states.reserve(docs_.size());
            for (const auto& [uri, state] : docs_) {
                if (uri == current_uri || !state)
                    continue;
                states.push_back(state);
            }
        }

        // Build outside map_mutex_: get_dynamic_index() may lazily walk an AST
        // for a newly opened buffer.  The generation check below prevents this
        // off-lock work from publishing a stale merge after a concurrent edit.
        auto merged = std::make_shared<SyntaxIndex>();
        for (const auto& state : states)
            merged->merge(get_dynamic_index(*state));

        std::lock_guard<std::mutex> lock(map_mutex_);
        if (generation != opened_files_index_generation_)
            continue;

        auto [it, inserted] = opened_files_index_cache_.emplace(current_uri, merged);
        (void)inserted;
        return it->second;
    }
}

CompilationSnapshot Analyzer::compilation_snapshot() const {
    std::lock_guard<std::mutex> lock(map_mutex_);

    CompilationSnapshot snapshot;
    snapshot.defines = defines_;
    snapshot.include_dirs = include_dirs_;

    std::unordered_set<std::string> seen_uris;
    std::unordered_set<std::string> seen_paths;

    for (const auto& [uri, state] : docs_) {
        if (!state)
            continue;

        snapshot.files.push_back(CompilationSourceFile{
            .uri = uri,
            .path = state->normalized_path,
            .text = std::shared_ptr<const std::string>(state, &state->text),
        });
        snapshot.open_uris.push_back(uri);
        seen_uris.insert(uri);
        seen_paths.insert(state->normalized_path);
    }

    for (const auto& configured_path : extra_files_) {
        const auto path = normalize_filesystem_path(configured_path);
        const auto path_string = path.string();
        const auto uri = uri_from_path(path);
        if (seen_uris.contains(uri) || seen_paths.contains(path_string))
            continue;

        snapshot.files.push_back(CompilationSourceFile{
            .uri = uri,
            .path = path_string,
            .text = nullptr,
        });
        seen_uris.insert(uri);
        seen_paths.insert(path_string);
    }

    return snapshot;
}

void Analyzer::set_semantic_diagnostics(
    std::unordered_map<std::string, std::vector<ParseDiagInfo>> diagnostics) {
    std::lock_guard<std::mutex> lock(map_mutex_);
    semantic_diagnostics_ = std::move(diagnostics);
}

void Analyzer::clear_semantic_diagnostics(const std::string& uri) {
    std::lock_guard<std::mutex> lock(map_mutex_);
    semantic_diagnostics_.erase(uri);
}

void Analyzer::clear_all_semantic_diagnostics() {
    std::lock_guard<std::mutex> lock(map_mutex_);
    semantic_diagnostics_.clear();
}

std::vector<ParseDiagInfo> Analyzer::semantic_diagnostics(const std::string& uri) const {
    std::lock_guard<std::mutex> lock(map_mutex_);
    const auto it = semantic_diagnostics_.find(uri);
    if (it == semantic_diagnostics_.end())
        return {};
    return it->second;
}

std::vector<std::string> Analyzer::semantic_diagnostic_uris() const {
    std::lock_guard<std::mutex> lock(map_mutex_);
    std::vector<std::string> uris;
    uris.reserve(semantic_diagnostics_.size());
    for (const auto& [uri, _] : semantic_diagnostics_)
        uris.push_back(uri);
    return uris;
}

namespace {

struct RtlIndexedInstance {
    InstanceEntry entry;
    std::string uri;
};

struct RtlIndexView {
    std::unordered_map<std::string, std::string> module_uris;
    std::vector<RtlIndexedInstance> instances;
};

void add_rtl_index_file(RtlIndexView& view, std::unordered_set<std::string>& seen_uris,
                        const std::string& uri, const SyntaxIndex& index) {
    if (!seen_uris.insert(uri).second)
        return;

    for (const auto& module : index.modules)
        view.module_uris.try_emplace(module.name, uri);

    for (const auto& instance : index.instances)
        view.instances.push_back(RtlIndexedInstance{.entry = instance, .uri = uri});
}

} // namespace

std::optional<RtlTreeNode> Analyzer::rtl_tree(const std::string& uri) const {
    auto state = get_state(uri);
    if (!state || !state->tree)
        return std::nullopt;
    const auto state_index = get_structural_index(*state);
    if (state_index.modules.empty())
        return std::nullopt;

    std::vector<std::pair<std::string, std::shared_ptr<const DocumentState>>> open_states;
    std::shared_ptr<const std::vector<ExtraFileInfo>> extra_snapshot;
    {
        std::lock_guard<std::mutex> lock(map_mutex_);
        open_states.reserve(docs_.size());
        for (const auto& [state_uri, state_snapshot] : docs_)
            open_states.emplace_back(state_uri, state_snapshot);
        if (!extra_file_snapshot_cache_)
            extra_file_snapshot_cache_ = build_extra_file_snapshot_locked();
        extra_snapshot = extra_file_snapshot_cache_;
    }

    RtlIndexView view;
    std::unordered_set<std::string> seen_uris;
    for (const auto& [state_uri, state_snapshot] : open_states) {
        if (state_snapshot && state_snapshot->tree)
            add_rtl_index_file(view, seen_uris, state_uri, get_structural_index(*state_snapshot));
    }
    for (const auto& extra : *extra_snapshot)
        add_rtl_index_file(view, seen_uris, extra.uri, extra.index);

    const auto* root = &state_index.modules.front();
    for (const auto& module : state_index.modules) {
        if (module.line > 0 && (root->line <= 0 || module.line < root->line))
            root = &module;
    }

    std::function<RtlTreeNode(const std::string&, size_t, std::unordered_set<std::string>&)> build =
        [&](const std::string& module_name, size_t depth,
            std::unordered_set<std::string>& seen) -> RtlTreeNode {
        auto module_it = view.module_uris.find(module_name);
        RtlTreeNode node{
            .name = module_name,
            .inst = {},
            .file = module_it != view.module_uris.end() ? module_it->second : std::string{},
            .children = {},
            .recursive = seen.contains(module_name),
        };
        if (node.recursive || module_it == view.module_uris.end())
            return node;
        if (depth >= kMaxRtlTreeDepth) {
            node.truncated = true;
            return node;
        }

        seen.insert(module_name);
        for (const auto& inst : view.instances) {
            if (inst.entry.parent_module != module_name)
                continue;
            if (inst.entry.module_name == module_name)
                continue;
            auto child = build(inst.entry.module_name, depth + 1, seen);
            child.inst = inst.entry.instance_name;
            node.children.push_back(std::move(child));
        }
        seen.erase(module_name);
        return node;
    };

    std::unordered_set<std::string> seen;
    return build(root->name, 0, seen);
}

std::optional<RtlTreeNode> Analyzer::rtl_tree_reverse(const std::string& uri) const {
    auto state = get_state(uri);
    if (!state || !state->tree)
        return std::nullopt;
    const auto state_index = get_structural_index(*state);
    if (state_index.modules.empty())
        return std::nullopt;

    std::vector<std::pair<std::string, std::shared_ptr<const DocumentState>>> open_states;
    std::shared_ptr<const std::vector<ExtraFileInfo>> extra_snapshot;
    {
        std::lock_guard<std::mutex> lock(map_mutex_);
        open_states.reserve(docs_.size());
        for (const auto& [state_uri, state_snapshot] : docs_)
            open_states.emplace_back(state_uri, state_snapshot);
        if (!extra_file_snapshot_cache_)
            extra_file_snapshot_cache_ = build_extra_file_snapshot_locked();
        extra_snapshot = extra_file_snapshot_cache_;
    }

    RtlIndexView view;
    std::unordered_set<std::string> seen_uris;
    for (const auto& [state_uri, state_snapshot] : open_states) {
        if (state_snapshot && state_snapshot->tree)
            add_rtl_index_file(view, seen_uris, state_uri, get_structural_index(*state_snapshot));
    }
    for (const auto& extra : *extra_snapshot)
        add_rtl_index_file(view, seen_uris, extra.uri, extra.index);

    const auto* target = &state_index.modules.front();
    for (const auto& module : state_index.modules) {
        if (module.line > 0 && (target->line <= 0 || module.line < target->line))
            target = &module;
    }

    struct ParentRef {
        std::string parent_module;
        std::string inst_name;
        std::string file_uri;
    };
    std::unordered_map<std::string, std::vector<ParentRef>> reverse_map;
    for (const auto& inst : view.instances) {
        if (inst.entry.parent_module.empty())
            continue;
        reverse_map[inst.entry.module_name].push_back(ParentRef{
            .parent_module = inst.entry.parent_module,
            .inst_name = inst.entry.instance_name,
            .file_uri = inst.uri,
        });
    }

    std::function<RtlTreeNode(const std::string&, size_t, std::unordered_set<std::string>&)> build =
        [&](const std::string& module_name, size_t depth,
            std::unordered_set<std::string>& seen) -> RtlTreeNode {
        auto module_it = view.module_uris.find(module_name);
        RtlTreeNode node{
            .name = module_name,
            .inst = {},
            .file = module_it != view.module_uris.end() ? module_it->second : std::string{},
            .children = {},
            .recursive = seen.contains(module_name),
        };
        if (node.recursive)
            return node;
        if (depth >= kMaxRtlTreeDepth) {
            node.truncated = true;
            return node;
        }

        seen.insert(module_name);
        auto refs = reverse_map.find(module_name);
        if (refs == reverse_map.end()) {
            seen.erase(module_name);
            return node;
        }

        for (const auto& parent : refs->second) {
            auto child = build(parent.parent_module, depth + 1, seen);
            child.inst = parent.inst_name;
            if (child.file.empty())
                child.file = parent.file_uri;
            node.children.push_back(std::move(child));
        }
        seen.erase(module_name);
        return node;
    };

    std::unordered_set<std::string> seen;
    return build(target->name, 0, seen);
}

void Analyzer::start_background_indexer_locked() const {
    if (background_indexer_.joinable())
        return;

    background_indexer_ = std::jthread([this](std::stop_token stop) {
        background_index_loop(stop);
    });
}

void Analyzer::schedule_background_reindex_locked() const {
    // A new generation invalidates parse results from older define/include/file
    // configurations.  The worker checks the generation again just before
    // committing each shard, so a slow parse can never overwrite newer project
    // state.
    ++background_generation_;
    background_pending_files_.clear();
    for (const auto& path : extra_files_)
        background_pending_files_.push_back(path);
    start_background_indexer_locked();
    background_cv_.notify_all();
}

void Analyzer::schedule_background_project_publish_locked() const {
    background_publish_requested_ = true;
    background_publish_due_time_ = Clock::now() +
        std::chrono::milliseconds(std::max(0, background_publish_debounce_ms_));
    start_background_indexer_locked();
    background_cv_.notify_all();
}

void Analyzer::background_index_loop(std::stop_token stop) const {
    while (!stop.stop_requested()) {
        std::string path_string;
        std::string uri;
        std::vector<std::string> defines;
        std::vector<std::string> include_dirs;
        std::vector<OpenTextOverlay> open_overlays;
        std::shared_ptr<const DocumentState> live_doc;
        uint64_t generation = 0;

        {
            std::unique_lock<std::mutex> lock(map_mutex_);
            background_cv_.wait(lock, stop, [&] {
                return stop.stop_requested() || !background_pending_files_.empty() ||
                       background_publish_requested_;
            });
            if (stop.stop_requested())
                break;

            if (background_publish_requested_ && background_pending_files_.empty()) {
                const auto now = Clock::now();
                if (background_publish_due_time_ > now) {
                    background_cv_.wait_until(lock, stop, background_publish_due_time_, [&] {
                        return stop.stop_requested() || !background_pending_files_.empty() ||
                               background_publish_due_time_ <= Clock::now();
                    });
                    continue;
                }

                background_publish_requested_ = false;
                auto publish_callback = publish_project_index_snapshot_locked();
                background_cv_.notify_all();
                lock.unlock();
                if (publish_callback)
                    publish_callback();
                continue;
            }

            path_string = std::move(background_pending_files_.front());
            background_pending_files_.pop_front();
            background_index_active_ = true;
            const auto path = normalize_filesystem_path(path_string);
            path_string = path.string();
            uri = uri_from_path(path);
            generation = background_generation_;
            defines = defines_;
            include_dirs = include_dirs_;
            open_overlays.reserve(docs_.size());
            for (const auto& [open_uri, open_state] : docs_) {
                if (!open_state || open_uri == uri)
                    continue;
                if (open_state->normalized_path == path_string)
                    continue;
                open_overlays.push_back(OpenTextOverlay{
                    .uri = open_uri,
                    .path = open_state->normalized_path,
                    .state = open_state,
                });
            }

            // Open buffers are already parsed from unsaved text by didOpen /
            // didChange.  Avoid reparsing stale disk contents, but also avoid
            // building the dynamic shard while map_mutex_ is held: that AST walk
            // can be noticeable for large RTL files and would otherwise block
            // unrelated request handlers.
            if (const auto doc = docs_.find(uri); doc != docs_.end() && doc->second) {
                live_doc = doc->second;
            }
        }

        if (live_doc) {
            // The queued path may represent an indirect include dependency
            // refresh, not a direct edit to this open document.  Reparse the
            // live text in the background so includes are resolved against the
            // latest open-buffer overlays, then atomically replace the stale
            // DocumentState if the user has not edited it meanwhile.
            auto reparsed_live_doc = make_state(uri, live_doc->text);
            auto live_index = get_dynamic_index(*reparsed_live_doc);
            std::lock_guard<std::mutex> lock(map_mutex_);
            if (generation == background_generation_) {
                if (const auto doc = docs_.find(uri); doc != docs_.end() && doc->second == live_doc) {
                    docs_[uri] = reparsed_live_doc;
                    invalidate_extra_snapshots_locked();
                    if (extra_file_set_.contains(path_string)) {
                        extra_cache_[uri] = ExtraFileCacheEntry{
                            .path = path_string,
                            .uri = uri,
                            .index = std::make_shared<SyntaxIndex>(std::move(live_index)),
                        };
                        invalidate_extra_snapshots_locked();
                        schedule_background_project_publish_locked();
                    }
                }
            }
            background_index_active_ = false;
            background_cv_.notify_all();
            continue;
        }

        auto state = make_file_state_with_options(path_string, defines, include_dirs,
                                                  open_overlays);
        if (stop.stop_requested() || !state || !state->tree) {
            std::lock_guard<std::mutex> lock(map_mutex_);
            background_index_active_ = false;
            background_cv_.notify_all();
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(map_mutex_);
            if (generation != background_generation_) {
                background_index_active_ = false;
                background_cv_.notify_all();
                continue;
            }

            // If the user opened/edited this file while the disk parse was in
            // flight, the live buffer is newer and must win. The didOpen /
            // didChange path builds and commits that live shard outside this
            // mutex, so do not build it here while holding map_mutex_.
            if (const auto doc = docs_.find(uri); doc != docs_.end() && doc->second) {
                background_index_active_ = false;
                background_cv_.notify_all();
                continue;
            }

            SyntaxIndex committed_index = state->index;

            extra_cache_[uri] = ExtraFileCacheEntry{
                .path = path_string,
                .uri = uri,
                .index = std::make_shared<SyntaxIndex>(std::move(committed_index)),
            };
            invalidate_extra_snapshots_locked();

            // ProjectIndex is an immutable view derived from per-file shards.
            // Do not publish after every single file while the initial .f cache
            // is warming: that would repeatedly notify downstream features for
            // partially warmed snapshots.  Request one debounced publish only
            // after the queue drains so [design].project_index_publish_debounce_ms
            // applies consistently to disk-backed reindex and live edit paths.
            if (background_pending_files_.empty())
                schedule_background_project_publish_locked();
            background_index_active_ = false;
            background_cv_.notify_all();
        }
    }
}

std::function<void()> Analyzer::publish_project_index_snapshot_locked() const {
    auto snapshot = std::make_shared<ProjectIndexSnapshot>();
    snapshot->shards.reserve(extra_cache_.size());

    for (const auto& [key, entry] : extra_cache_) {
        if (!entry.index)
            continue;

        snapshot->shards.push_back(ProjectIndexSnapshot::Shard{
            .path = entry.path,
            .uri = entry.uri,
            .index = entry.index,
        });

        // Lightweight global module lookup.  Keep first definition wins to
        // preserve the historical merge behavior for duplicate module names.
        for (size_t i = 0; i < entry.index->modules.size(); ++i) {
            const auto& module = entry.index->modules[i];
            snapshot->module_by_name.try_emplace(module.name, ProjectIndexModuleRef{
                .shard = entry.index,
                .module_index = i,
            });
        }
    }

    project_index_snapshot_cache_ = std::move(snapshot);

    // Return the callback to the caller instead of invoking it here.  This
    // function is called while map_mutex_ is held; endpoint notifications can
    // block on client or logging behavior and must not run under the analyzer
    // mutex.
    return project_index_publish_callback_;
}

void Analyzer::clear_project_index_snapshot_locked() const {
    project_index_snapshot_cache_.reset();
}

void Analyzer::update_extra_cache_for_live_state_locked(
    std::shared_ptr<const DocumentState> state, SyntaxIndex index) {
    if (!state)
        return;

    const auto path_string = state->normalized_path;
    const auto uri = state->uri;

    // Only files explicitly listed in the design filelist participate in the
    // project index.  Random open buffers should not pollute project-wide
    // completion for the configured design.
    if (!extra_file_set_.contains(path_string))
        return;

    extra_cache_[uri] = ExtraFileCacheEntry{
        .path = path_string,
        .uri = uri,
        .index = std::make_shared<SyntaxIndex>(std::move(index)),
    };
    invalidate_extra_snapshots_locked();

    // The per-file shard changed, so the published merged project snapshot is
    // stale until the background indexer republishes it.  Publish asynchronously
    // instead of merging synchronously on the edit/open path; this preserves the
    // HPC-friendly rule that request/edit handlers do not rebuild whole-project
    // state inline.
    schedule_background_project_publish_locked();

}
