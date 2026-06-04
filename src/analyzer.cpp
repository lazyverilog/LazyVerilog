#include "analyzer.hpp"
#include "dynamic_file_index.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
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
    const char* value = std::getenv("LAZYVERILOG_TRACE_PERF");
    return value && *value && std::string_view(value) != "0";
}

using Clock = std::chrono::steady_clock;

void log_perf(std::string_view label, Clock::time_point start) {
    if (!perf_trace_enabled())
        return;
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start);
    std::cerr << "[lazyverilog][perf] " << label << ": " << elapsed.count() << "us\n";
}

} // namespace

static std::string file_name_to_uri(std::string_view file_name, const std::string& fallback_uri);
static std::string path_to_uri(const std::filesystem::path& path);
static std::optional<std::string> read_file_text_optional(const std::filesystem::path& path);
static std::filesystem::path normalize_path(const std::string& path);

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

static std::shared_ptr<DocumentState>
make_file_state_with_options(const std::filesystem::path& path,
                             const std::vector<std::string>& defines,
                             const std::vector<std::string>& include_dirs) {
    const auto start = Clock::now();
    const auto norm = normalize_path(path);
    const std::string uri = path_to_uri(norm);

    auto sm = std::make_unique<slang::SourceManager>();
    add_include_dirs(*sm, include_dirs);
    slang::parsing::PreprocessorOptions ppo;
    ppo.predefines = defines;
    slang::Bag bag;
    bag.set(ppo);

    auto tree_or_error = slang::syntax::SyntaxTree::fromFile(norm.string(), *sm, bag);
    if (!tree_or_error)
        return nullptr;

    auto text = read_file_text_optional(norm);
    auto state = std::make_shared<DocumentState>(uri, text.value_or(std::string{}), nullptr);
    state->source_manager = std::move(sm);
    state->tree = std::move(*tree_or_error);
    if (state->tree)
        state->index = SyntaxIndex::build(*state->tree, state->text, IndexDepth::Declarations);
    collect_parse_diagnostics(*state, uri);
    log_perf("make_file_state " + uri, start);
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
    {
        std::lock_guard<std::mutex> lock(map_mutex_);
        defines = defines_;
        include_dirs = include_dirs_;
    }

    auto sm = std::make_unique<slang::SourceManager>();
    add_include_dirs(*sm, include_dirs);
    slang::parsing::PreprocessorOptions ppo;
    ppo.predefines = defines;
    slang::Bag bag;
    bag.set(ppo);
    auto tree = slang::syntax::SyntaxTree::fromText(
        std::string_view(text), *sm, std::string_view(uri), std::string_view(path), bag);
    auto state = std::make_shared<DocumentState>(uri, text, nullptr);
    state->source_manager = std::move(sm);
    state->tree = std::move(tree);
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

std::shared_ptr<DocumentState> Analyzer::make_file_state(const std::filesystem::path& path) const {
    std::vector<std::string> defines;
    std::vector<std::string> include_dirs;
    {
        std::lock_guard<std::mutex> lock(map_mutex_);
        defines = defines_;
        include_dirs = include_dirs_;
    }
    return make_file_state_with_options(path, defines, include_dirs);
}

void Analyzer::open(const std::string& uri, const std::string& text) {
    auto state = make_state(uri, text);
    std::lock_guard<std::mutex> lock(map_mutex_);
    docs_[uri] = state;
    update_extra_cache_for_live_state_locked(state);
}

void Analyzer::change(const std::string& uri, const std::string& text) {
    auto state = make_state(uri, text);
    std::lock_guard<std::mutex> lock(map_mutex_);
    docs_[uri] = state;
    update_extra_cache_for_live_state_locked(state);
}

void Analyzer::close(const std::string& uri) {
    std::lock_guard<std::mutex> lock(map_mutex_);
    docs_.erase(uri);
    // If the closed file is also in the filelist cache, replace its live shard
    // with a disk-backed parse in the background.  Keeping this asynchronous is
    // important for large buffers: closing a split should not synchronously
    // parse an include-heavy RTL file on the UI path.
    for (auto& entry : extra_cache_) {
        if (entry.uri != uri)
            continue;
        background_pending_files_.push_back(entry.path);
        start_background_indexer_locked();
        background_cv_.notify_all();
        break;
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

static int to_lsp_line(int one_based_line) { return one_based_line > 0 ? one_based_line - 1 : 0; }

static std::string path_to_uri(const std::filesystem::path& path) {
    return "file://" + std::filesystem::absolute(path).lexically_normal().string();
}

static std::string file_name_to_uri(std::string_view file_name, const std::string& fallback_uri) {
    if (file_name.empty())
        return fallback_uri;

    std::string file(file_name);
    if (file.starts_with("file://"))
        return file;
    return path_to_uri(file);
}

struct IndexedFile {
    std::string uri;
    SyntaxIndex index;
};

static std::optional<IndexedFile> build_index_for_file(const std::filesystem::path& path) {
    slang::SourceManager sm;
    auto tree_or_error = slang::syntax::SyntaxTree::fromFile(path.string(), sm);
    if (!tree_or_error)
        return std::nullopt;

    return IndexedFile{path_to_uri(path), SyntaxIndex::build(**tree_or_error)};
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
                                                               const Location& loc) {
    for (const auto& ref : index.references) {
        if (!ref.symbol_id || ref.symbol_debug.starts_with("name:"))
            continue;
        // ReferenceEntry no longer stores a URI string directly.  It stores a
        // compact FileID that is local to the SyntaxIndex shard, so always
        // resolve through the shard's file table before comparing locations.
        const auto ref_uri = index.source_uri(ref.file_id);
        if (!ref_uri.empty() && ref_uri != loc.uri)
            continue;
        if (to_lsp_line(ref.line) == loc.line && ref.col == loc.col)
            return ref.symbol_debug;
    }
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

static std::string read_file_text(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

static std::optional<std::string> read_file_text_optional(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return std::nullopt;
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

static std::filesystem::path normalize_path(const std::string& path) {
    std::error_code ec;
    auto absolute = std::filesystem::absolute(std::filesystem::path(path), ec);
    if (ec)
        absolute = std::filesystem::path(path);
    return absolute.lexically_normal();
}

static std::string trim_copy(std::string text) {
    auto first =
        std::find_if_not(text.begin(), text.end(), [](unsigned char c) { return std::isspace(c); });
    auto last = std::find_if_not(text.rbegin(), text.rend(), [](unsigned char c) {
                    return std::isspace(c);
                }).base();
    if (first >= last)
        return {};
    return std::string(first, last);
}

static bool hover_fragment_edge_is_wordlike(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$' || c == '`';
}

static bool hover_needs_space_between_fragments(std::string_view previous,
                                                std::string_view next) {
    if (previous.empty() || next.empty())
        return false;

    const char prev = previous.back();
    const char curr = next.front();

    // Keep adjacent identifiers / keywords / literals readable:
    //
    //     bit signed
    //     virtual interface axi_if
    //     struct packed
    //
    // Without this, a trivia-free token stream would become "bitsigned" or
    // "virtualinterface".
    if (hover_fragment_edge_is_wordlike(prev) && hover_fragment_edge_is_wordlike(curr))
        return true;

    // Packed and unpacked dimensions are syntactically bracketed, but hover is
    // documentation for humans.  Prefer "logic [31:0]" over "logic[31:0]".
    if (hover_fragment_edge_is_wordlike(prev) && curr == '[')
        return true;

    // Struct / union / enum bodies are easier to read with a separator:
    // "struct packed { ... }" instead of "struct packed{ ... }".
    if (hover_fragment_edge_is_wordlike(prev) && curr == '{')
        return true;

    // Lists should not collapse after commas when comments/trivia are stripped.
    if (prev == ',')
        return true;

    return false;
}

static std::optional<std::string>
source_text_for_hover_range(const slang::SourceManager& sm, slang::SourceRange range) {
    if (!range.start().valid() || !range.end().valid())
        return std::nullopt;
    if (range.start().buffer() != range.end().buffer())
        return std::nullopt;

    const auto source = sm.getSourceText(range.start().buffer());
    const size_t begin = range.start().offset();
    const size_t end = range.end().offset();
    if (begin > end || end > source.size())
        return std::nullopt;

    return std::string(source.substr(begin, end - begin));
}

static bool same_source_range(slang::SourceRange lhs, slang::SourceRange rhs) {
    return lhs.start() == rhs.start() && lhs.end() == rhs.end();
}

static std::string render_hover_token_text(const slang::SourceManager& sm,
                                           const slang::parsing::Token& token,
                                           std::optional<slang::SourceRange>& last_macro_range) {
    if (sm.isMacroLoc(token.location())) {
        const auto expansion_range = sm.getExpansionRange(token.location());

        // A single macro invocation can expand to several syntax tokens.  Those
        // tokens all point back to the same invocation range in the user's
        // source.  Hover should show the invocation once:
        //
        //     logic [`RANGE] value;   // `RANGE might expand to 7:0
        //            ^^^^^^          // show `RANGE, not `RANGE`RANGE`RANGE
        //
        // For ordinary macro constants used as part of a larger range, this
        // still preserves the exact source spelling:
        //
        //     logic [`WIDTH-1:0] value;  // show `WIDTH-1:0, not 32-1:0
        if (last_macro_range && same_source_range(*last_macro_range, expansion_range))
            return {};
        last_macro_range = expansion_range;

        if (auto text = source_text_for_hover_range(sm, expansion_range))
            return *text;
    } else {
        last_macro_range.reset();
    }

    return std::string(token.rawText());
}

static std::string render_hover_syntax_text(const slang::SourceManager& sm,
                                            const slang::syntax::SyntaxNode& node) {
    std::string text;
    std::optional<slang::SourceRange> last_macro_range;
    for (auto it = node.tokens_begin(); it != node.tokens_end(); ++it) {
        const auto token = *it;
        if (!token || token.isMissing())
            continue;

        const auto fragment = render_hover_token_text(sm, token, last_macro_range);
        if (fragment.empty())
            continue;

        if (hover_needs_space_between_fragments(text, fragment))
            text += ' ';
        text += fragment;
    }
    return trim_copy(std::move(text));
}

static std::string render_hover_dimensions(
    const slang::SourceManager& sm,
    const slang::syntax::SyntaxList<slang::syntax::VariableDimensionSyntax>& dimensions) {
    std::string text;
    for (const auto* dimension : dimensions) {
        if (!dimension)
            continue;

        const auto rendered = render_hover_syntax_text(sm, *dimension);
        if (rendered.empty())
            continue;

        if (hover_needs_space_between_fragments(text, rendered))
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

static std::optional<SymbolInfo> find_macro_info_in_file(const std::filesystem::path& path,
                                                         const std::string& name) {
    slang::SourceManager sm;
    auto tree_or_error = slang::syntax::SyntaxTree::fromFile(path.string(), sm);
    if (!tree_or_error)
        return std::nullopt;
    return find_macro_info(**tree_or_error, path_to_uri(path), name);
}

static std::optional<Location> find_macro_definition_in_file(const std::filesystem::path& path,
                                                             const std::string& name) {
    slang::SourceManager sm;
    auto tree_or_error = slang::syntax::SyntaxTree::fromFile(path.string(), sm);
    if (!tree_or_error)
        return std::nullopt;
    return find_macro_definition(**tree_or_error, path_to_uri(path), name);
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

static std::optional<Location>
find_subroutine_argument_definition_in_file(const std::filesystem::path& path,
                                            const std::string& subroutine_name,
                                            const std::string& argument_name) {
    slang::SourceManager sm;
    auto tree_or_error = slang::syntax::SyntaxTree::fromFile(path.string(), sm);
    if (!tree_or_error)
        return std::nullopt;
    return find_subroutine_argument_definition(**tree_or_error, path_to_uri(path), subroutine_name,
                                               argument_name);
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

static std::optional<Location>
find_generic_definition_in_file(const std::filesystem::path& path, const std::string& name,
                                const std::string& preferred_module) {
    slang::SourceManager sm;
    auto tree_or_error = slang::syntax::SyntaxTree::fromFile(path.string(), sm);
    if (!tree_or_error)
        return std::nullopt;
    return find_generic_definition(**tree_or_error, path_to_uri(path), name, preferred_module);
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
            if (!result)
                visitDefault(node);
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
            if (!result)
                visitDefault(node);
        }

        void handle(const slang::syntax::ImplicitAnsiPortSyntax& node) {
            if (!node.declarator)
                return;
            std::string detail;
            if (node.header) {
                if (const auto* variable =
                        node.header->as_if<slang::syntax::VariablePortHeaderSyntax>()) {
                    detail = token_text(variable->direction);
                    auto type = render_hover_syntax_text(sm, *variable->dataType);
                    if (!type.empty())
                        detail += (detail.empty() ? "" : " ") + type;
                } else if (const auto* net =
                               node.header->as_if<slang::syntax::NetPortHeaderSyntax>()) {
                    detail = token_text(net->direction);
                    auto type = render_hover_syntax_text(sm, *net->dataType);
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
                auto type = render_hover_syntax_text(sm, *variable->dataType);
                if (!type.empty())
                    detail += (detail.empty() ? "" : " ") + type;
            } else if (const auto* net = node.header->as_if<slang::syntax::NetPortHeaderSyntax>()) {
                detail = token_text(net->direction);
                auto type = render_hover_syntax_text(sm, *net->dataType);
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
            auto base_type = render_hover_syntax_text(sm, *node.type);
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
            auto type = render_hover_syntax_text(sm, *node.type);
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
            auto base_type = render_hover_syntax_text(sm, *node.type);
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
            auto base_type = render_hover_syntax_text(sm, *node.type);
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
            auto detail = render_hover_syntax_text(sm, *node.type);
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
                auto type = render_hover_syntax_text(sm, *node.dataType);
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
                    auto return_type = render_hover_syntax_text(sm, *node.prototype->returnType);
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
            set_from_token(node.name, "typedef", render_hover_syntax_text(sm, *node.type));
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

static std::optional<SymbolInfo> symbol_info_from_definition_file(const std::filesystem::path& path,
                                                                  const std::string& name,
                                                                  const Location& definition) {
    slang::SourceManager sm;
    auto tree_or_error = slang::syntax::SyntaxTree::fromFile(path.string(), sm);
    if (!tree_or_error)
        return std::nullopt;
    return symbol_info_from_definition(**tree_or_error, path_to_uri(path), name, definition);
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
    auto extra_files = extra_file_snapshots();

    if (target.kind == DefinitionTargetKind::Macro) {
        if (auto info = find_macro_info(*state->tree, uri, target.name))
            return info;
        for (const auto& extra : extra_files) {
            if (!extra.state || !extra.state->tree)
                continue;
            if (auto info = find_macro_info(*extra.state->tree, extra.uri, target.name))
                return info;
        }
        return SymbolInfo{
            .name = target.name, .kind = "macro", .detail = "(empty)", .line = line, .col = col};
    }

    auto definition = definition_of(uri, line, col);
    if (definition) {
        std::string name = target.name.empty() ? ident : target.name;
        if (definition->uri == uri) {
            if (auto info = symbol_info_from_definition(*state->tree, uri, name, *definition))
                return info;
        } else {
            for (const auto& extra : extra_files) {
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

    auto extra = extra_file_snapshots();
    // Remove the current document from extra to avoid searching it twice
    // (definition_of_state already searches it as the primary doc).
    extra.erase(std::remove_if(extra.begin(), extra.end(),
                               [&uri](const ExtraFileInfo& e) { return e.uri == uri; }),
                extra.end());
    auto result = definition_of_state(*state, uri, line, col, extra);
    log_perf("definition_of " + uri + ":" + std::to_string(line) + ":" + std::to_string(col),
             start);
    return result;
}

std::optional<Location>
Analyzer::definition_of_state(const DocumentState& state, const std::string& uri, int line, int col,
                              const std::vector<ExtraFileInfo>& extra_files) const {
    if (!state.tree)
        return std::nullopt;

    auto target = definition_target_at(*state.tree, uri, line, col);

    if (target.kind == DefinitionTargetKind::None || target.name.empty()) {
        auto ident = extract_ident_span(state.text, line, col);
        if (!ident || !is_backtick_identifier(state.text, line, ident->start_col))
            return std::nullopt;

        if (auto loc = find_macro_definition(*state.tree, uri, ident->text))
            return loc;
        for (const auto& extra : extra_files) {
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
            if (auto loc = find_module_definition(extra.index, extra.uri, target.module_name))
                return loc;
        }
        return std::nullopt;
    }

    if (auto loc = find_generic_definition(*state.tree, uri, target.name, target.scope_module))
        return loc;
    for (const auto& extra : extra_files) {
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
    if (!target || !state)
        return {};
    const auto target_info = state->tree ? definition_target_at(*state->tree, uri, line, col)
                                         : DefinitionTarget{};
    auto target_def = definition_of_state(*state, uri, line, col, extra_files);
    if (!target_def && target_info.kind == DefinitionTargetKind::Instance) {
        for (const auto& extra : extra_index_snapshots()) {
            if ((target_def = find_module_definition(extra.index, extra.uri,
                                                     target_info.module_name)))
                break;
        }
    } else if (!target_def && target_info.kind == DefinitionTargetKind::NamedPort) {
        for (const auto& extra : extra_index_snapshots()) {
            if ((target_def = find_port_definition(extra.index, extra.uri,
                                                   target_info.module_name, target_info.name)))
                break;
        }
    } else if (!target_def && target_info.kind == DefinitionTargetKind::NamedParameter) {
        for (const auto& extra : extra_index_snapshots()) {
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
        auto current_structural_index = build_current_ast_structural_index(*state);
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
            for (const auto& extra : extra_index_snapshots()) {
                if (extra.uri != target_def->uri)
                    continue;
                if (auto id = symbol_id_for_index_location(extra.index, *target_def))
                    target_symbol_debug = *id;
                break;
            }
        }
    }
    const SymbolID target_symbol_id = SymbolID::from_canonical(target_symbol_debug);

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
            const auto open_index = build_current_ast_structural_index(*state);
            for (const auto& ref : open_index.references) {
                if (ref.symbol_id == target_symbol_id)
                    add_indexed_reference(state_uri, open_index, ref);
            }
        } else {
            visit_tree(*state->tree, state_uri, resolve_snapshot);
        }
    }

    // Closed project files are represented by compact reference-occurrence
    // shards.  We intentionally do not load or walk their full SyntaxTrees here.
    for (const auto& extra : extra_index_snapshots()) {
        if (open_uris.contains(extra.uri))
            continue;
        if (!target_symbol_id)
            continue;
        for (const auto& ref : extra.index.references) {
            if (ref.symbol_id != target_symbol_id)
                continue;

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

void Analyzer::set_defines(const std::vector<std::string>& defines) {
    std::lock_guard<std::mutex> lock(map_mutex_);
    defines_ = defines;
    // Invalidate extra-file cache so reopened files pick up the new defines.
    extra_cache_.clear();
    clear_extra_project_index_locked();
    if (!extra_files_.empty())
        schedule_background_reindex_locked();
}

void Analyzer::set_include_dirs(const std::vector<std::string>& include_dirs) {
    std::lock_guard<std::mutex> lock(map_mutex_);
    include_dirs_.clear();
    include_dirs_.reserve(include_dirs.size());
    for (const auto& dir : include_dirs)
        include_dirs_.push_back(normalize_path(dir).string());

    // Include paths affect parsing every explicit filelist source.  Clear the
    // cache even if the filelist itself did not change, otherwise a newly added
    // UVM include directory would not be visible until the next source edit.
    extra_cache_.clear();
    clear_extra_project_index_locked();
    if (!extra_files_.empty())
        schedule_background_reindex_locked();
}

void Analyzer::set_extra_files(const std::vector<std::string>& paths,
                               const std::string& filelist_path) {
    std::lock_guard<std::mutex> lock(map_mutex_);
    filelist_path_ = filelist_path;
    extra_files_.clear();
    extra_cache_.clear();
    extra_files_.reserve(paths.size());
    for (const auto& path : paths)
        extra_files_.push_back(normalize_path(path).string());
    clear_extra_project_index_locked();
    if (filelist_path_.empty())
        refresh_extra_cache_locked();
    else
        schedule_background_reindex_locked();
}

std::vector<ExtraFileInfo> Analyzer::extra_file_snapshots() const {
    const auto start = Clock::now();
    std::lock_guard<std::mutex> lock(map_mutex_);
    // Do not poll the .f file mtime on request paths.  The filelist is treated
    // as configuration loaded at startup / config reload.  This avoids metadata
    // I/O in HPC environments and keeps edits to listed open buffers
    // incremental through update_extra_cache_for_live_state_locked().
    if (!extra_files_.empty() && extra_cache_.size() < extra_files_.size() &&
        filelist_path_.empty())
        refresh_extra_cache_locked();

    std::vector<ExtraFileInfo> result;
    result.reserve(extra_cache_.size());
    for (const auto& entry : extra_cache_) {
        // Closed project files intentionally have no DocumentState here.  They
        // are represented only by the compact SyntaxIndex shard.  If the file
        // is open, attach the live state so AST-only features can inspect the
        // unsaved buffer without keeping closed project ASTs alive.
        if (const auto it = docs_.find(entry.uri); it != docs_.end()) {
            result.push_back(ExtraFileInfo{
                .path = entry.path,
                .uri = entry.uri,
                .state = it->second,
                .index = build_dynamic_file_index(*it->second),
            });
            continue;
        }
        result.push_back(ExtraFileInfo{
            .path = entry.path,
            .uri = entry.uri,
            .state = nullptr,
            .index = entry.index,
        });
    }
    log_perf("extra_file_snapshots files=" + std::to_string(result.size()), start);
    return result;
}

std::vector<ExtraIndexInfo> Analyzer::extra_index_snapshots() const {
    const auto start = Clock::now();
    std::lock_guard<std::mutex> lock(map_mutex_);

    std::vector<ExtraIndexInfo> result;
    result.reserve(extra_cache_.size());
    for (const auto& entry : extra_cache_) {
        result.push_back(ExtraIndexInfo{
            .path = entry.path,
            .uri = entry.uri,
            .index = entry.index,
        });
    }
    log_perf("extra_index_snapshots files=" + std::to_string(result.size()), start);
    return result;
}

void Analyzer::merge_extra_file_modules(SyntaxIndex& index) const {
    for (const auto& extra : extra_index_snapshots())
        index.merge(extra.index);
}

std::shared_ptr<const SyntaxIndex> Analyzer::extra_project_index() const {
    const auto start = Clock::now();
    std::lock_guard<std::mutex> lock(map_mutex_);

    // Request path is read-only: the background/index-update path publishes
    // immutable merged ProjectIndex snapshots.  If the worker has not published
    // yet, return an empty snapshot rather than merging shards synchronously.
    if (!extra_project_index_cache_)
        extra_project_index_cache_ = std::make_shared<SyntaxIndex>();
    log_perf("extra_project_index files=" + std::to_string(extra_cache_.size()), start);
    return extra_project_index_cache_;
}

std::shared_ptr<const SyntaxIndex>
Analyzer::opened_files_index(const std::string& current_uri) const {
    std::lock_guard<std::mutex> lock(map_mutex_);
    auto merged = std::make_shared<SyntaxIndex>();
    for (const auto& [uri, state] : docs_) {
        if (uri == current_uri || !state)
            continue;
        merged->merge(build_dynamic_file_index(*state));
    }
    return merged;
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

        std::string path = uri;
        if (path.starts_with("file://"))
            path = path.substr(7);

        snapshot.files.push_back(CompilationSourceFile{
            .uri = uri,
            .path = normalize_path(path).string(),
            .text = state->text,
        });
        snapshot.open_uris.push_back(uri);
        seen_uris.insert(uri);
        seen_paths.insert(normalize_path(path).string());
    }

    for (const auto& configured_path : extra_files_) {
        const auto path = normalize_path(configured_path);
        const auto path_string = path.string();
        const auto uri = path_to_uri(path);
        if (seen_uris.contains(uri) || seen_paths.contains(path_string))
            continue;

        snapshot.files.push_back(CompilationSourceFile{
            .uri = uri,
            .path = path_string,
            .text = std::nullopt,
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
    const auto state_index = build_current_ast_structural_index(*state);
    if (state_index.modules.empty())
        return std::nullopt;

    RtlIndexView view;
    std::unordered_set<std::string> seen_uris;
    for_each_state([&](const std::string& state_uri, const auto& state_snapshot) {
        if (state_snapshot && state_snapshot->tree)
            add_rtl_index_file(view, seen_uris, state_uri, build_current_ast_structural_index(*state_snapshot));
    });
    for (const auto& extra : extra_file_snapshots())
        add_rtl_index_file(view, seen_uris, extra.uri, extra.index);

    const auto* root = &state_index.modules.front();
    for (const auto& module : state_index.modules) {
        if (module.line > 0 && (root->line <= 0 || module.line < root->line))
            root = &module;
    }

    std::function<RtlTreeNode(const std::string&, const std::unordered_set<std::string>&)> build =
        [&](const std::string& module_name,
            const std::unordered_set<std::string>& seen) -> RtlTreeNode {
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

        auto next_seen = seen;
        next_seen.insert(module_name);
        for (const auto& inst : view.instances) {
            if (inst.entry.parent_module != module_name)
                continue;
            if (inst.entry.module_name == module_name)
                continue;
            auto child = build(inst.entry.module_name, next_seen);
            child.inst = inst.entry.instance_name;
            node.children.push_back(std::move(child));
        }
        return node;
    };

    return build(root->name, {});
}

std::optional<RtlTreeNode> Analyzer::rtl_tree_reverse(const std::string& uri) const {
    auto state = get_state(uri);
    if (!state || !state->tree)
        return std::nullopt;
    const auto state_index = build_current_ast_structural_index(*state);
    if (state_index.modules.empty())
        return std::nullopt;

    RtlIndexView view;
    std::unordered_set<std::string> seen_uris;
    for_each_state([&](const std::string& state_uri, const auto& state_snapshot) {
        if (state_snapshot && state_snapshot->tree)
            add_rtl_index_file(view, seen_uris, state_uri, build_current_ast_structural_index(*state_snapshot));
    });
    for (const auto& extra : extra_file_snapshots())
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

    std::function<RtlTreeNode(const std::string&, const std::unordered_set<std::string>&)> build =
        [&](const std::string& module_name,
            const std::unordered_set<std::string>& seen) -> RtlTreeNode {
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

        auto next_seen = seen;
        next_seen.insert(module_name);
        auto refs = reverse_map.find(module_name);
        if (refs == reverse_map.end())
            return node;

        for (const auto& parent : refs->second) {
            auto child = build(parent.parent_module, next_seen);
            child.inst = parent.inst_name;
            if (child.file.empty())
                child.file = parent.file_uri;
            node.children.push_back(std::move(child));
        }
        return node;
    };

    return build(target->name, {});
}

void Analyzer::refresh_extra_cache_locked() const {
    const auto start = Clock::now();
    std::vector<ExtraFileCacheEntry> refreshed;
    refreshed.reserve(extra_files_.size());

    for (const auto& configured_path : extra_files_) {
        const auto path = normalize_path(configured_path);
        const auto path_string = path.string();
        const auto uri = path_to_uri(path);

        // If this file is open in the editor, its DocumentState is the
        // authoritative shard.  This makes project-wide completion see unsaved
        // edits in b.sv when completing from top.sv, without reparsing every
        // filelist source.
        if (const auto doc = docs_.find(uri); doc != docs_.end() && doc->second) {
            refreshed.push_back(ExtraFileCacheEntry{
                .path = path_string,
                .uri = uri,
                .index = build_dynamic_file_index(*doc->second),
            });
            continue;
        }

        auto existing = std::find_if(
            extra_cache_.begin(), extra_cache_.end(), [&](const ExtraFileCacheEntry& entry) {
                return entry.path == path_string;
            });
        if (existing != extra_cache_.end()) {
            refreshed.push_back(*existing);
            continue;
        }

        auto state = make_file_state_with_options(path, defines_, include_dirs_);
        if (!state || !state->tree)
            continue;

        refreshed.push_back(ExtraFileCacheEntry{
            .path = path_string,
            .uri = uri,
            .index = state->index,
        });
    }

    extra_cache_ = std::move(refreshed);
    publish_extra_project_index_locked();
    log_perf("refresh_extra_cache files=" + std::to_string(extra_cache_.size()), start);
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
    start_background_indexer_locked();
    background_cv_.notify_all();
}

void Analyzer::background_index_loop(std::stop_token stop) const {
    while (!stop.stop_requested()) {
        std::string path_string;
        std::string uri;
        std::vector<std::string> defines;
        std::vector<std::string> include_dirs;
        uint64_t generation = 0;

        {
            std::unique_lock<std::mutex> lock(map_mutex_);
            background_cv_.wait(lock, stop, [&] {
                return stop.stop_requested() || !background_pending_files_.empty() ||
                       background_publish_requested_;
            });
            if (stop.stop_requested())
                break;

            if (background_pending_files_.empty() && background_publish_requested_) {
                background_publish_requested_ = false;
                publish_extra_project_index_locked();
                continue;
            }

            path_string = std::move(background_pending_files_.front());
            background_pending_files_.pop_front();
            const auto path = normalize_path(path_string);
            path_string = path.string();
            uri = path_to_uri(path);
            generation = background_generation_;
            defines = defines_;
            include_dirs = include_dirs_;

            // Open buffers are already parsed from unsaved text by didOpen /
            // didChange.  Commit that live shard immediately and avoid wasting
            // background CPU reparsing stale disk contents.
            if (const auto doc = docs_.find(uri); doc != docs_.end() && doc->second) {
                auto live_index = build_dynamic_file_index(*doc->second);
                auto existing = std::find_if(
                    extra_cache_.begin(), extra_cache_.end(), [&](const ExtraFileCacheEntry& e) {
                        return e.path == path_string || e.uri == uri;
                    });
                if (existing != extra_cache_.end()) {
                    existing->path = path_string;
                    existing->uri = uri;
                    existing->index = std::move(live_index);
                } else {
                    extra_cache_.push_back(ExtraFileCacheEntry{
                        .path = path_string,
                        .uri = uri,
                        .index = std::move(live_index),
                    });
                }
                background_publish_requested_ = true;
                continue;
            }
        }

        auto state = make_file_state_with_options(path_string, defines, include_dirs);
        if (stop.stop_requested() || !state || !state->tree)
            continue;

        {
            std::lock_guard<std::mutex> lock(map_mutex_);
            if (generation != background_generation_)
                continue;

            // If the user opened/edited this file while the disk parse was in
            // flight, the live buffer is newer and must win.
            SyntaxIndex committed_index = state->index;
            if (const auto doc = docs_.find(uri); doc != docs_.end() && doc->second) {
                committed_index = build_dynamic_file_index(*doc->second);
            }

            auto existing = std::find_if(
                extra_cache_.begin(), extra_cache_.end(), [&](const ExtraFileCacheEntry& e) {
                    return e.path == path_string || e.uri == uri;
                });
            if (existing != extra_cache_.end()) {
                existing->path = path_string;
                existing->uri = uri;
                existing->index = std::move(committed_index);
            } else {
                extra_cache_.push_back(ExtraFileCacheEntry{
                    .path = path_string,
                    .uri = uri,
                    .index = std::move(committed_index),
                });
            }

            // ProjectIndex is an immutable merged view derived from shards.
            // Publish from the background worker so request handlers never pay
            // the project-wide shard merge cost.
            publish_extra_project_index_locked();
        }
    }
}

void Analyzer::publish_extra_project_index_locked() const {
    auto merged = std::make_shared<SyntaxIndex>();
    for (const auto& entry : extra_cache_) {
        merged->merge(entry.index);
    }
    extra_project_index_cache_ = std::move(merged);
}

void Analyzer::clear_extra_project_index_locked() const {
    extra_project_index_cache_.reset();
}

void Analyzer::update_extra_cache_for_live_state_locked(
    std::shared_ptr<const DocumentState> state) {
    if (!state)
        return;

    std::string path_text = state->uri;
    if (path_text.starts_with("file://"))
        path_text = path_text.substr(7);
    const auto path = normalize_path(path_text);
    const auto path_string = path.string();
    const auto uri = path_to_uri(path);

    // Only files explicitly listed in the design filelist participate in the
    // project index.  Random open buffers should not pollute project-wide
    // completion for the configured design.
    const auto listed = std::find(extra_files_.begin(), extra_files_.end(), path_string);
    if (listed == extra_files_.end())
        return;

    auto existing = std::find_if(extra_cache_.begin(), extra_cache_.end(),
                                 [&](const ExtraFileCacheEntry& entry) {
                                     return entry.path == path_string || entry.uri == uri;
                                 });
    auto live_index = build_dynamic_file_index(*state);
    if (existing != extra_cache_.end()) {
        existing->path = path_string;
        existing->uri = uri;
        existing->index = std::move(live_index);
    } else {
        extra_cache_.push_back(ExtraFileCacheEntry{
            .path = path_string,
            .uri = uri,
            .index = std::move(live_index),
        });
    }

    // This is the Option-B shard replacement point: b.sv's shard is replaced
    // when b.sv receives didOpen/didChange.  The merged project view is
    // published asynchronously by the background worker; request handlers keep
    // seeing the previous snapshot until the worker finishes the merge.
    schedule_background_project_publish_locked();
}
