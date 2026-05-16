#include "analyzer.hpp"
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
#include <slang/syntax/AllSyntax.h>
#include <slang/parsing/Preprocessor.h>
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
    auto sm = std::make_unique<slang::SourceManager>();
    slang::parsing::PreprocessorOptions ppo;
    ppo.predefines = defines_;
    slang::Bag bag;
    bag.set(ppo);
    auto tree = slang::syntax::SyntaxTree::fromText(std::string_view(text), *sm,
                                                    std::string_view(uri), std::string_view(path),
                                                    bag);
    auto state = std::make_shared<DocumentState>(uri, text, nullptr);
    state->source_manager = std::move(sm);
    state->tree = std::move(tree);
    if (state->tree)
        state->index = SyntaxIndex::build(*state->tree, state->text);
    // Format diagnostics immediately while the SyntaxTree arena is alive.
    // Do NOT copy slang::Diagnostic objects — their ConstantValue args can
    // contain internal pointers that are not safely copyable.
    if (state->tree) {
        const auto& diags = state->tree->diagnostics();
        auto& sm = state->tree->sourceManager();
        slang::DiagnosticEngine engine(sm);
        for (const auto& d : diags) {
            ParseDiagInfo info;
            try {
                auto loc = d.location.valid() ? sm.getFullyExpandedLoc(d.location) : d.location;
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
            state->parse_diagnostics.push_back(std::move(info));
        }
    }
    log_perf("make_state " + uri, start);
    return state;
}

void Analyzer::open(const std::string& uri, const std::string& text) {
    auto state = make_state(uri, text);
    std::lock_guard<std::mutex> lock(map_mutex_);
    docs_[uri] = state;
}

void Analyzer::change(const std::string& uri, const std::string& text) {
    auto state = make_state(uri, text);
    std::lock_guard<std::mutex> lock(map_mutex_);
    docs_[uri] = state;
}

void Analyzer::close(const std::string& uri) {
    std::lock_guard<std::mutex> lock(map_mutex_);
    docs_.erase(uri);
    // If the closed file is also in extra_cache_, invalidate its mtime so
    // refresh_extra_cache_locked() re-parses it from disk on the next access.
    // Without this, extra_cache_ holds the pre-open snapshot — missing any
    // edits the user made while the file was open.
    for (auto& entry : extra_cache_) {
        if (entry.uri == uri) {
            entry.mtime = std::nullopt;
            break;
        }
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
    const int line = to_lsp_line(module.line);
    return Location{uri, line, module.col, line, module.col + (int)module.name.size()};
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

    const int line = to_lsp_line(port->line);
    return Location{uri, line, port->col, line, port->col + (int)port->name.size()};
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

static std::optional<std::filesystem::file_time_type>
file_mtime(const std::filesystem::path& path) {
    std::error_code ec;
    auto mtime = std::filesystem::last_write_time(path, ec);
    if (ec)
        return std::nullopt;
    return mtime;
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
        if (macro && macro->name.valueText() == name)
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

    void handle(const slang::syntax::TypedefDeclarationSyntax& node) { maybe_set(node.name); }
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

        void handle(const slang::syntax::ImplicitAnsiPortSyntax& node) {
            if (!node.declarator)
                return;
            std::string detail;
            if (node.header) {
                if (const auto* variable =
                        node.header->as_if<slang::syntax::VariablePortHeaderSyntax>()) {
                    detail = token_text(variable->direction);
                    auto type = trim_copy(variable->dataType->toString());
                    if (!type.empty())
                        detail += (detail.empty() ? "" : " ") + type;
                } else if (const auto* net =
                               node.header->as_if<slang::syntax::NetPortHeaderSyntax>()) {
                    detail = token_text(net->direction);
                    auto type = trim_copy(net->dataType->toString());
                    if (!type.empty())
                        detail += (detail.empty() ? "" : " ") + type;
                }
            }
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
                auto type = trim_copy(variable->dataType->toString());
                if (!type.empty())
                    detail += (detail.empty() ? "" : " ") + type;
            } else if (const auto* net = node.header->as_if<slang::syntax::NetPortHeaderSyntax>()) {
                detail = token_text(net->direction);
                auto type = trim_copy(net->dataType->toString());
                if (!type.empty())
                    detail += (detail.empty() ? "" : " ") + type;
            }
            for (const auto* declarator : node.declarators) {
                if (declarator)
                    set_from_token(declarator->name, "port", detail);
            }
        }

        void handle(const slang::syntax::DataDeclarationSyntax& node) {
            for (const auto* declarator : node.declarators) {
                if (declarator)
                    set_from_token(declarator->name, "variable", "");
            }
        }

        void handle(const slang::syntax::NetDeclarationSyntax& node) {
            auto detail = token_text(node.netType);
            auto type = trim_copy(node.type->toString());
            if (!type.empty())
                detail += (detail.empty() ? "" : " ") + type;
            for (const auto* declarator : node.declarators) {
                if (declarator)
                    set_from_token(declarator->name, "net", detail);
            }
        }

        void handle(const slang::syntax::LocalVariableDeclarationSyntax& node) {
            for (const auto* declarator : node.declarators) {
                if (declarator)
                    set_from_token(declarator->name, "variable", "");
            }
        }

        void handle(const slang::syntax::ParameterDeclarationSyntax& node) {
            auto detail = trim_copy(node.type->toString());
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
                auto type = trim_copy(node.dataType->toString());
                if (!type.empty())
                    detail += (detail.empty() ? "" : " ") + type;
            }
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
                    auto return_type = trim_copy(node.prototype->returnType->toString());
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
            set_from_token(node.name, "typedef", trim_copy(node.type->toString()));
        }
    };

    auto built_index = prebuilt_index ? SyntaxIndex{} : SyntaxIndex::build(tree);
    const auto& index = prebuilt_index ? *prebuilt_index : built_index;
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

static bool token_contains_position(const slang::SourceManager& sm,
                                    const slang::parsing::Token& token, int line, int col) {
    return token && token.location().valid() &&
           contains_position(sm, visible_range_for_token(sm, token), line, col);
}

enum class DefinitionTargetKind {
    None,
    Instance,
    NamedPort,
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
    int line;
    int col;
    DefinitionTarget target;
    std::string current_module;

    DefinitionTargetVisitor(const slang::SourceManager& sm, int line, int col)
        : sm(sm), line(line), col(col) {}

    bool found() const { return target.kind != DefinitionTargetKind::None; }

    void handle(const slang::syntax::ModuleDeclarationSyntax& node) {
        if (token_contains_position(sm, node.header->name, line, col)) {
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
        if (token_contains_position(sm, node.type, line, col)) {
            target.kind = DefinitionTargetKind::Generic;
            target.name = std::string(node.type.valueText());
            target.scope_module = current_module;
            return;
        }

        const std::string module_name(node.type.valueText());
        for (const auto* instance : node.instances) {
            if (!instance)
                continue;
            if (instance->decl && token_contains_position(sm, instance->decl->name, line, col)) {
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
                if (named && token_contains_position(sm, named->name, line, col)) {
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
            if (token_contains_position(sm, named->name, line, col)) {
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
        if (identifier && token_contains_position(sm, identifier->identifier, line, col)) {
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
            if (!contains_position(sm, sm.getExpansionRange(token.location()), line, col))
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
            token_contains_position(sm, token, line, col)) {
            target.kind = DefinitionTargetKind::Generic;
            target.name = std::string(token.valueText());
            target.scope_module = current_module;
        }
    }
};

static DefinitionTarget definition_target_at(const slang::syntax::SyntaxTree& tree, int line,
                                             int col) {
    DefinitionTargetVisitor visitor(tree.sourceManager(), line, col);
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

    const auto& idx = state->index;
    auto target = definition_target_at(*state->tree, line, col);
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

    for (const auto& m : idx.modules) {
        if (m.name == ident)
            return SymbolInfo{.name = ident,
                              .kind = "module",
                              .detail = "module",
                              .doc = module_doc_from_entry(m),
                              .line = m.line,
                              .col = m.col};
        for (const auto& p : m.ports)
            if (p.name == ident)
                return SymbolInfo{.name = ident,
                                  .kind = "port",
                                  .detail = p.direction + (p.type.empty() ? "" : " " + p.type),
                                  .line = p.line,
                                  .col = p.col};
    }
    for (const auto& inst : idx.instances)
        if (inst.instance_name == ident)
            return SymbolInfo{.name = ident,
                              .kind = "instance",
                              .detail = inst.module_name,
                              .line = inst.line,
                              .col = 0};

    auto definition = definition_of(uri, line, col);
    if (definition) {
        std::string name = target.name.empty() ? ident : target.name;
        if (definition->uri == uri) {
            if (auto info = symbol_info_from_definition(*state->tree, uri, name, *definition, &idx))
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
        int line;
        int col;
        std::optional<IdentifierAtPosition> result;

        Visitor(const slang::SourceManager& sm, int line, int col) : sm(sm), line(line), col(col) {}

        void visitToken(slang::parsing::Token token) {
            if (result || !token || token.kind != slang::parsing::TokenKind::Identifier)
                return;
            if (!token_contains_position(sm, token, line, col))
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

    Visitor visitor(state->tree->sourceManager(), line, col);
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

    auto target = definition_target_at(*state.tree, line, col);
    const auto& idx = state.index;

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
        if (auto loc = find_port_definition(idx, uri, target.module_name, target.name))
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
        if (auto loc = find_module_definition(idx, uri, target.module_name))
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
    auto target_def = definition_of(uri, line, col);
    if (!target || !target_def)
        return {};

    auto extra_files = extra_file_snapshots();

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

    std::set<std::string> open_uris;
    std::unordered_map<std::string, std::shared_ptr<const DocumentState>> open_state_by_uri;
    for (const auto& [state_uri, state] : open_states) {
        open_uris.insert(state_uri);
        open_state_by_uri[state_uri] = state;
    }

    auto resolve_snapshot = [&](const std::string& candidate_uri, int ref_line,
                                int ref_col) -> std::optional<Location> {
        if (auto it = open_state_by_uri.find(candidate_uri); it != open_state_by_uri.end()) {
            if (!it->second)
                return std::nullopt;
            return definition_of_state(*it->second, candidate_uri, ref_line, ref_col, extra_files);
        }
        for (const auto& extra : extra_files) {
            if (extra.uri != candidate_uri || !extra.state)
                continue;
            return definition_of_state(*extra.state, candidate_uri, ref_line, ref_col, extra_files);
        }
        return std::nullopt;
    };

    for (const auto& [state_uri, state] : open_states) {
        if (!state || !state->tree)
            continue;
        visit_tree(*state->tree, state_uri, resolve_snapshot);
    }

    for (const auto& extra : extra_files) {
        if (open_uris.contains(extra.uri) || !extra.state || !extra.state->tree)
            continue;
        auto resolver = [&](const std::string& candidate_uri, int ref_line,
                            int ref_col) -> std::optional<Location> {
            if (candidate_uri != extra.uri)
                return std::nullopt;
            return resolve_snapshot(candidate_uri, ref_line, ref_col);
        };
        visit_tree(*extra.state->tree, extra.uri, resolver);
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
}

void Analyzer::set_extra_files(const std::vector<std::string>& paths,
                               const std::string& filelist_path) {
    std::lock_guard<std::mutex> lock(map_mutex_);
    filelist_path_ = filelist_path;
    extra_files_.clear();
    extra_files_.reserve(paths.size());
    for (const auto& path : paths)
        extra_files_.push_back(normalize_path(path).string());
    refresh_extra_cache_locked();
    // Cache current filelist mtime so the next extra_file_snapshots() call
    // skips the refresh (nothing changed since this forced full parse).
    if (!filelist_path_.empty())
        filelist_mtime_ = file_mtime(std::filesystem::path(filelist_path_));
}

std::vector<ExtraFileInfo> Analyzer::extra_file_snapshots() const {
    const auto start = Clock::now();
    std::lock_guard<std::mutex> lock(map_mutex_);
    // Verible-style: stat() only the .f filelist file (1 syscall per request)
    // instead of every individual extra file (N syscalls). Refresh the cache
    // only when the filelist itself changes. On NFS/HPC this keeps per-request
    // overhead at O(1) instead of O(N * stat_latency).
    if (!filelist_path_.empty()) {
        const auto current_mtime = file_mtime(std::filesystem::path(filelist_path_));
        if (current_mtime != filelist_mtime_) {
            refresh_extra_cache_locked();
            filelist_mtime_ = current_mtime;
        }
    }

    std::vector<ExtraFileInfo> result;
    result.reserve(extra_cache_.size());
    for (const auto& entry : extra_cache_) {
        if (!entry.state)
            continue;
        // If the file is currently open in the editor, use its live docs_ state
        // so callers see the latest edits rather than the stale disk parse.
        // (Previously this skipped open files entirely, making split-open buffers
        // invisible to cross-file lookups like go-to-def, hover, references.)
        if (const auto it = docs_.find(entry.uri); it != docs_.end()) {
            result.push_back(ExtraFileInfo{
                .path = entry.path,
                .uri = entry.uri,
                .state = it->second,
                .index = it->second->index,
            });
            continue;
        }
        result.push_back(ExtraFileInfo{
            .path = entry.path,
            .uri = entry.uri,
            .state = entry.state,
            .index = entry.index,
        });
    }
    log_perf("extra_file_snapshots files=" + std::to_string(result.size()), start);
    return result;
}

void Analyzer::merge_extra_file_modules(SyntaxIndex& index) const {
    for (const auto& extra : extra_file_snapshots()) {
        const size_t base = index.modules.size();
        index.modules.insert(index.modules.end(), extra.index.modules.begin(),
                             extra.index.modules.end());
        for (size_t i = base; i < index.modules.size(); ++i)
            index.module_by_name.try_emplace(index.modules[i].name, i);
    }
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
    if (!state || !state->tree || state->index.modules.empty())
        return std::nullopt;

    RtlIndexView view;
    std::unordered_set<std::string> seen_uris;
    for_each_state([&](const std::string& state_uri, const auto& state_snapshot) {
        if (state_snapshot && state_snapshot->tree)
            add_rtl_index_file(view, seen_uris, state_uri, state_snapshot->index);
    });
    for (const auto& extra : extra_file_snapshots())
        add_rtl_index_file(view, seen_uris, extra.uri, extra.index);

    const auto* root = &state->index.modules.front();
    for (const auto& module : state->index.modules) {
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
    if (!state || !state->tree || state->index.modules.empty())
        return std::nullopt;

    RtlIndexView view;
    std::unordered_set<std::string> seen_uris;
    for_each_state([&](const std::string& state_uri, const auto& state_snapshot) {
        if (state_snapshot && state_snapshot->tree)
            add_rtl_index_file(view, seen_uris, state_uri, state_snapshot->index);
    });
    for (const auto& extra : extra_file_snapshots())
        add_rtl_index_file(view, seen_uris, extra.uri, extra.index);

    const auto* target = &state->index.modules.front();
    for (const auto& module : state->index.modules) {
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
        const auto mtime = file_mtime(path);
        if (!mtime)
            continue;

        auto existing = std::find_if(
            extra_cache_.begin(), extra_cache_.end(), [&](const ExtraFileCacheEntry& entry) {
                return entry.path == path_string && entry.mtime == mtime && entry.state;
            });
        if (existing != extra_cache_.end()) {
            refreshed.push_back(*existing);
            continue;
        }

        auto text = read_file_text_optional(path);
        if (!text)
            continue;

        const auto uri = path_to_uri(path);
        auto state = make_state(uri, *text);
        if (!state || !state->tree)
            continue;

        refreshed.push_back(ExtraFileCacheEntry{
            .path = path_string,
            .uri = uri,
            .mtime = mtime,
            .state = state,
            .index = state->index,
        });
    }

    extra_cache_ = std::move(refreshed);
    log_perf("refresh_extra_cache files=" + std::to_string(extra_cache_.size()), start);
}
