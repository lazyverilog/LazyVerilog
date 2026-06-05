#include "connect.hpp"
#include "../dynamic_file_index.hpp"

#include <slang/syntax/AllSyntax.h>
#include <slang/syntax/SyntaxTree.h>
#include <slang/syntax/SyntaxVisitor.h>
#include <slang/text/SourceManager.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace {

struct FileView {
    std::string uri;
    std::string path;
    // Text is present immediately for open buffers because it can include
    // unsaved edits.  For closed project files it starts empty and is filled
    // lazily only when Connect must compute a TextEdit in that exact file.
    // This avoids reading every .f entry on ConnectInfo/preview/apply requests.
    mutable std::string text;
    mutable bool text_loaded{false};
    SyntaxIndex index;
    // Non-null only for open/current buffers.  Per project architecture, these
    // live SyntaxTree snapshots are authoritative for edit-local facts such as
    // declarations in unsaved text.  Closed/filelist files keep only SyntaxIndex.
    std::shared_ptr<const DocumentState> state;
};

struct ResolvedInst {
    std::string path;          // Hierarchical path, e.g. top.u_mid.u_leaf.
    std::string inst_name;     // Instance token at this hierarchy level.
    std::string module_name;   // Module type instantiated by inst_name.
    std::string parent_path;   // Hierarchical path of containing module instance/root.
    std::string parent_module; // Module that textually contains this instance.
    std::string file_uri;      // File that contains the instance text.
    InstanceEntry entry;
};

struct TextEdit {
    std::string uri;
    int sl{0};
    int sc{0};
    int el{0};
    int ec{0};
    std::string text;
};

struct PreviewEdit {
    std::string file;
    int line{1};
    std::string description;
    bool warning{false};
};

static std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char ch : s) {
        switch (ch) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20) {
                char buf[7];
                std::snprintf(buf, sizeof(buf), "\\u%04x", ch);
                out += buf;
            } else {
                out += ch;
            }
        }
    }
    return out;
}

static std::string q(const std::string& s) { return "\"" + json_escape(s) + "\""; }

static std::string basename_from_uri(const std::string& uri) {
    const auto pos = uri.find_last_of('/');
    return pos == std::string::npos ? uri : uri.substr(pos + 1);
}

static std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> lines;
    std::stringstream ss(text);
    std::string line;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        lines.push_back(line);
    }
    if (text.empty() || (!text.empty() && text.back() == '\n'))
        lines.push_back("");
    return lines;
}

static std::string trim(std::string s) {
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

static std::string read_file_text_best_effort(const std::string& path) {
    if (path.empty())
        return {};
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static const std::string& file_text(const FileView& file) {
    if (!file.text_loaded) {
        file.text = read_file_text_best_effort(file.path);
        file.text_loaded = true;
    }
    return file.text;
}

static std::vector<FileView> collect_files(const Analyzer& analyzer, const std::string& uri) {
    std::vector<FileView> files;
    std::unordered_set<std::string> seen;

    // Open buffers are authoritative: they contain unsaved edits.  Derive the
    // structural view from the live SyntaxTree on demand instead of relying on
    // a didChange-built current-file index.
    analyzer.for_each_state([&](const std::string& state_uri,
                                const std::shared_ptr<const DocumentState>& state) {
        if (!state || !seen.insert(state_uri).second)
            return;
        files.push_back(FileView{.uri = state_uri,
                                 .path = {},
                                 .text = state->text,
                                 .text_loaded = true,
                                 .index = get_structural_index(*state),
                                 .state = state});
    });

    // Filelist entries fill in library modules / sibling modules. If an extra
    // file is also open, skip it so the open-buffer version wins.  Closed
    // project files remain index-authoritative structurally.  Keep only their
    // path here; edit helpers call file_text() lazily for the small subset of
    // closed files that actually receive WorkspaceEdits.
    for (const auto& extra : analyzer.extra_file_snapshots()) {
        if (!seen.insert(extra.uri).second)
            continue;
        if (extra.state) {
            files.push_back(FileView{.uri = extra.uri,
                                     .path = extra.path,
                                     .text = extra.state->text,
                                     .text_loaded = true,
                                     .index = get_structural_index(*extra.state),
                                     .state = extra.state});
        } else {
            files.push_back(FileView{.uri = extra.uri,
                                     .path = extra.path,
                                     .text = {},
                                     .text_loaded = false,
                                     .index = extra.index,
                                     .state = nullptr});
        }
    }

    // Be defensive for command calls that arrive before didOpen is processed.
    if (!seen.contains(uri)) {
        if (auto state = analyzer.get_state(uri))
            files.push_back(FileView{.uri = uri,
                                     .path = {},
                                     .text = state->text,
                                     .text_loaded = true,
                                     .index = get_structural_index(*state),
                                     .state = state});
    }
    return files;
}

static const ModuleEntry* find_module(const std::vector<FileView>& files, const std::string& name,
                                      const FileView** file_out = nullptr) {
    for (const auto& file : files) {
        auto it = file.index.module_by_name.find(name);
        if (it == file.index.module_by_name.end())
            continue;
        if (file_out)
            *file_out = &file;
        return &file.index.modules[it->second];
    }
    return nullptr;
}

static std::unordered_map<std::string, ResolvedInst>
build_hierarchy(const std::vector<FileView>& files) {
    std::unordered_map<std::string, ResolvedInst> resolved;
    std::unordered_map<std::string, std::vector<std::pair<const FileView*, const InstanceEntry*>>>
        by_parent_module;
    std::unordered_set<std::string> instantiated_modules;

    for (const auto& file : files) {
        for (const auto& inst : file.index.instances) {
            by_parent_module[inst.parent_module].push_back({&file, &inst});
            instantiated_modules.insert(inst.module_name);
        }
    }

    struct Root { std::string path; std::string module; };
    std::vector<Root> frontier;
    for (const auto& file : files) {
        for (const auto& module : file.index.modules) {
            if (!instantiated_modules.contains(module.name))
                frontier.push_back({module.name, module.name});
        }
    }
    // In a library-only or cyclic design every module can appear instantiated;
    // seed all modules so ConnectInfo still has useful instance choices.
    if (frontier.empty()) {
        for (const auto& file : files)
            for (const auto& module : file.index.modules)
                frontier.push_back({module.name, module.name});
    }

    std::set<std::string> visited_roots;
    for (size_t idx = 0; idx < frontier.size(); ++idx) {
        const auto root = frontier[idx];
        if (!visited_roots.insert(root.path + "|" + root.module).second)
            continue;

        auto children = by_parent_module.find(root.module);
        if (children == by_parent_module.end())
            continue;
        for (const auto& [file, inst] : children->second) {
            const std::string child_path = root.path + "." + inst->instance_name;
            if (resolved.contains(child_path))
                continue;
            resolved.emplace(child_path, ResolvedInst{.path = child_path,
                                                      .inst_name = inst->instance_name,
                                                      .module_name = inst->module_name,
                                                      .parent_path = root.path,
                                                      .parent_module = root.module,
                                                      .file_uri = file->uri,
                                                      .entry = *inst});
            frontier.push_back({child_path, inst->module_name});
        }
    }
    return resolved;
}


struct DesignLookup {
    std::unordered_map<std::string, const FileView*> module_file_by_name;
    std::unordered_map<std::string, const ModuleEntry*> module_by_name;
};

static DesignLookup build_design_lookup(const std::vector<FileView>& files) {
    DesignLookup lookup;
    for (const auto& file : files) {
        for (const auto& module : file.index.modules) {
            // First definition wins, matching find_module().  Open buffers are
            // collected before filelist shards, so unsaved current text remains
            // authoritative when a file also appears in the project filelist.
            lookup.module_file_by_name.emplace(module.name, &file);
            lookup.module_by_name.emplace(module.name, &module);
        }
        // Do not pre-group every instance in the project here.  Connect
        // preview/apply resolves only the requested source/destination routes,
        // so instance scans are deferred to the file that owns the current
        // parent module at each hierarchy step.
    }
    return lookup;
}

static std::vector<std::string> split_hier_path(const std::string& path) {
    std::vector<std::string> parts;
    std::stringstream ss(path);
    std::string part;
    while (std::getline(ss, part, '.')) {
        if (!part.empty())
            parts.push_back(part);
    }
    return parts;
}

static std::optional<std::vector<ResolvedInst>> resolve_instance_route(
    const DesignLookup& lookup, const std::string& hierarchical_inst_path, std::string& error) {
    const auto parts = split_hier_path(hierarchical_inst_path);
    if (parts.size() < 2) {
        error = "instance '" + hierarchical_inst_path + "' not found";
        return std::nullopt;
    }

    const std::string root_module = parts.front();
    if (!lookup.module_by_name.contains(root_module)) {
        error = "root module '" + root_module + "' not found";
        return std::nullopt;
    }

    std::vector<ResolvedInst> route;
    std::string parent_path = root_module;
    std::string parent_module = root_module;
    for (size_t i = 1; i < parts.size(); ++i) {
        const auto module_file_it = lookup.module_file_by_name.find(parent_module);
        if (module_file_it == lookup.module_file_by_name.end()) {
            error = "module '" + parent_module + "' not found";
            return std::nullopt;
        }

        // Route-local hierarchy construction: inspect only the indexed file
        // that owns the current parent module, find the requested child
        // instance, then jump to that child module for the next segment.
        // This avoids building `top.*` paths for unrelated branches.
        const FileView* inst_file = module_file_it->second;
        const InstanceEntry* inst_entry = nullptr;
        for (const auto& inst : inst_file->index.instances) {
            if (inst.parent_module == parent_module && inst.instance_name == parts[i]) {
                inst_entry = &inst;
                break;
            }
        }
        if (!inst_entry) {
            error = "instance '" + hierarchical_inst_path + "' not found";
            return std::nullopt;
        }

        const std::string child_path = parent_path + "." + inst_entry->instance_name;
        route.push_back(ResolvedInst{.path = child_path,
                                     .inst_name = inst_entry->instance_name,
                                     .module_name = inst_entry->module_name,
                                     .parent_path = parent_path,
                                     .parent_module = parent_module,
                                     .file_uri = inst_file->uri,
                                     .entry = *inst_entry});
        parent_path = child_path;
        parent_module = inst_entry->module_name;
    }
    return route;
}

static std::unordered_map<std::string, ResolvedInst> route_hierarchy_map(
    const std::vector<ResolvedInst>& source_route, const std::vector<ResolvedInst>& dest_route) {
    std::unordered_map<std::string, ResolvedInst> hierarchy;
    for (const auto& inst : source_route)
        hierarchy.emplace(inst.path, inst);
    for (const auto& inst : dest_route)
        hierarchy.emplace(inst.path, inst);
    return hierarchy;
}

static std::optional<PortEntry> port_on_module(const std::vector<FileView>& files,
                                               const std::string& module_name,
                                               const std::string& port_name) {
    if (const auto* module = find_module(files, module_name)) {
        auto it = module->port_by_name.find(port_name);
        if (it != module->port_by_name.end())
            return module->ports[it->second];
    }
    return std::nullopt;
}

static std::string decl_type_for_port(const PortEntry& port) {
    return port.decl_type.empty() ? port.type : port.decl_type;
}

static std::string signal_decl_type_for_port(const PortEntry& port) {
    // This is precomputed by SyntaxIndex from slang syntax node kinds.  Do not
    // infer net-vs-variable from textual prefixes here: `wire`, dimensions,
    // typedef names, and macros are source text, while the choice to generate a
    // variable bridge signal is a syntax fact.
    return port.signal_decl_type.empty() ? decl_type_for_port(port) : port.signal_decl_type;
}

static std::optional<NamedPortConn> connection_for(const InstanceEntry& inst,
                                                   const std::string& port_name) {
    for (const auto& conn : inst.connections) {
        if (conn.port_name == port_name)
            return conn;
    }
    return std::nullopt;
}

static std::pair<int, int> offset_to_pos(const std::string& text, size_t off) {
    off = std::min(off, text.size());
    int line = 0, col = 0;
    for (size_t i = 0; i < off; ++i) {
        if (text[i] == '\n') {
            ++line;
            col = 0;
        } else {
            ++col;
        }
    }
    return {line, col};
}

static size_t offset_from_position(const std::string& text, int line, int col) {
    line = std::max(line, 0);
    col = std::max(col, 0);

    size_t offset = 0;
    int current_line = 0;
    while (offset < text.size() && current_line < line) {
        if (text[offset] == '\n')
            ++current_line;
        ++offset;
    }

    size_t line_end = offset;
    while (line_end < text.size() && text[line_end] != '\n')
        ++line_end;
    return std::min(offset + static_cast<size_t>(col), line_end);
}

static bool syntax_fragment_edge_is_wordlike(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$' || c == '`';
}

static bool needs_space_between_syntax_fragments(std::string_view previous,
                                                 std::string_view next) {
    if (previous.empty() || next.empty())
        return false;
    const char prev = previous.back();
    const char curr = next.front();
    if (syntax_fragment_edge_is_wordlike(prev) && syntax_fragment_edge_is_wordlike(curr))
        return true;
    if (syntax_fragment_edge_is_wordlike(prev) && (curr == '[' || curr == '{'))
        return true;
    if (prev == ',')
        return true;
    return false;
}

static std::string token_text(const slang::parsing::Token& token) {
    if (!token || token.isMissing())
        return {};
    if (token.rawText().empty())
        return std::string(token.valueText());
    return std::string(token.rawText());
}

static std::string render_syntax_text(const slang::syntax::SyntaxNode& node) {
    std::string text;
    for (auto it = node.tokens_begin(); it != node.tokens_end(); ++it) {
        const auto fragment = token_text(*it);
        if (fragment.empty())
            continue;
        if (needs_space_between_syntax_fragments(text, fragment))
            text += ' ';
        text += fragment;
    }
    return trim(std::move(text));
}

static std::string append_declarator_dimensions(std::string type,
                                                const slang::syntax::DeclaratorSyntax& declarator) {
    for (const auto* dimension : declarator.dimensions) {
        if (!dimension)
            continue;
        const auto rendered = render_syntax_text(*dimension);
        if (!rendered.empty())
            type += (type.empty() ? "" : " ") + rendered;
    }
    return trim(std::move(type));
}

static std::string type_from_data_declaration(const slang::syntax::DataDeclarationSyntax& node,
                                              const slang::syntax::DeclaratorSyntax& declarator) {
    return append_declarator_dimensions(render_syntax_text(*node.type), declarator);
}

static std::string type_from_net_declaration(const slang::syntax::NetDeclarationSyntax& node,
                                             const slang::syntax::DeclaratorSyntax& declarator) {
    std::string type = token_text(node.netType);
    const auto data_type = render_syntax_text(*node.type);
    if (!data_type.empty())
        type += (type.empty() ? "" : " ") + data_type;
    return append_declarator_dimensions(std::move(type), declarator);
}

static std::pair<int, int> token_pos(const slang::SourceManager& sm,
                                     const slang::parsing::Token& token) {
    if (!token || !token.location().valid())
        return {0, 0};
    const auto line = sm.getLineNumber(token.location());
    const auto col = sm.getColumnNumber(token.location());
    return {line > 0 ? static_cast<int>(line) - 1 : 0,
            col > 0 ? static_cast<int>(col) - 1 : 0};
}

static std::pair<int, int> range_end_pos(const slang::SourceManager& sm,
                                         slang::SourceRange range) {
    if (!range.end().valid())
        return {0, 0};
    return offset_to_pos(std::string(sm.getSourceText(range.end().buffer())), range.end().offset());
}

struct DeclInfo {
    std::string name;
    std::string type;
    int start_line{0};
    int start_col{0};
    int end_line{0};
    int end_col{0};
    size_t declarator_count{0};
};

struct ModuleDeclVisitor : public slang::syntax::SyntaxVisitor<ModuleDeclVisitor> {
    const slang::SourceManager& sm;
    std::string target_module;
    std::vector<DeclInfo> decls;

    ModuleDeclVisitor(const slang::SourceManager& sm, std::string target_module)
        : sm(sm), target_module(std::move(target_module)) {}

    void add_decl_at_range_start(const slang::parsing::Token& name_token, const std::string& type,
                                 slang::SourceRange range, size_t count) {
        if (!range.start().valid() || range.start().buffer() != range.end().buffer())
            return;
        auto [start_line, start_col] = offset_to_pos(
            std::string(sm.getSourceText(range.start().buffer())), range.start().offset());
        auto [end_line, end_col] = range_end_pos(sm, range);
        decls.push_back(DeclInfo{.name = token_text(name_token),
                                 .type = type,
                                 .start_line = start_line,
                                 .start_col = start_col,
                                 .end_line = end_line,
                                 .end_col = end_col,
                                 .declarator_count = count});
    }

    void add_data_declaration(const slang::syntax::DataDeclarationSyntax& node) {
        const size_t count = node.declarators.size();
        for (const auto* decl : node.declarators) {
            if (!decl)
                continue;
            add_decl_at_range_start(decl->name, type_from_data_declaration(node, *decl),
                                    node.sourceRange(), count);
        }
    }

    void add_net_declaration(const slang::syntax::NetDeclarationSyntax& node) {
        const size_t count = node.declarators.size();
        for (const auto* decl : node.declarators) {
            if (!decl)
                continue;
            add_decl_at_range_start(decl->name, type_from_net_declaration(node, *decl),
                                    node.sourceRange(), count);
        }
    }

    void handle(const slang::syntax::ModuleDeclarationSyntax& node) {
        const std::string module_name = token_text(node.header->name);
        if (!target_module.empty() && module_name != target_module)
            return;

        // Only module-scope declarations are relevant for Connect/Interface
        // bridge wires.  Do not recursively visit procedural blocks, functions,
        // tasks, classes, or generate bodies: inserting a bridge declaration
        // after a local variable inside those scopes would be syntactically and
        // semantically wrong.  Closed-file data uses SyntaxIndex; open buffers
        // use this direct AST member pass to preserve unsaved declarations.
        for (const auto* member : node.members) {
            if (!member)
                continue;
            if (const auto* data = member->as_if<slang::syntax::DataDeclarationSyntax>())
                add_data_declaration(*data);
            else if (const auto* net = member->as_if<slang::syntax::NetDeclarationSyntax>())
                add_net_declaration(*net);
        }
    }
};

static std::vector<DeclInfo> ast_declarations_for_module(const FileView& file,
                                                         const std::string& module_name) {
    if (!file.state || !file.state->tree)
        return {};
    ModuleDeclVisitor visitor(file.state->tree->sourceManager(), module_name);
    file.state->tree->root().visit(visitor);
    return std::move(visitor.decls);
}

static bool declared_signal(const FileView& file, const std::string& module_name,
                            const std::string& name) {
    if (name.empty())
        return false;

    if (file.state && file.state->tree) {
        for (const auto& decl : ast_declarations_for_module(file, module_name)) {
            if (decl.name == name)
                return true;
        }
    }

    // Closed/project files are represented by SyntaxIndex.  This is also a
    // conservative fallback if an open buffer temporarily lacks a SyntaxTree.
    for (const auto& value : file.index.values) {
        if (value.name == name &&
            (module_name.empty() || value.parent_scope.empty() || value.parent_scope == module_name))
            return true;
    }
    return false;
}

static int wire_insert_line(const FileView& file, const std::string& module_name) {
    int last_decl_line = -1;
    for (const auto& decl : ast_declarations_for_module(file, module_name))
        last_decl_line = std::max(last_decl_line, decl.end_line);
    if (last_decl_line >= 0)
        return last_decl_line + 1;

    for (const auto& value : file.index.values) {
        // SyntaxIndex.values also contains ports and parameter-port-list
        // entries.  Those are not valid anchors for inserting an internal
        // bridge wire: with an ANSI header they can be physically inside the
        // module header, causing the wire to be inserted into the port list.
        // Only real module-item declarations should move the insertion point
        // past the header fallback.
        if ((value.kind == "variable" || value.kind == "net") &&
            (module_name.empty() || value.parent_scope == module_name) && value.line > 0)
            last_decl_line = std::max(last_decl_line, value.line - 1);
    }
    if (last_decl_line >= 0)
        return last_decl_line + 1;

    // No existing declarations in this module. Insert immediately after this
    // module's header semicolon using the AST/SyntaxIndex-derived edit range.
    auto mod_it = file.index.module_by_name.find(module_name);
    if (mod_it != file.index.module_by_name.end()) {
        const auto& module = file.index.modules[mod_it->second];
        if (module.header_semi_line >= 0)
            return module.header_semi_line + 1;
    }

    return 0;
}

static std::string signal_type_in_file(const FileView& file, const std::string& module_name,
                                       const std::string& sig) {
    if (sig.empty())
        return "";
    for (const auto& decl : ast_declarations_for_module(file, module_name)) {
        if (decl.name == sig)
            return decl.type;
    }
    for (const auto& value : file.index.values) {
        if (value.name == sig &&
            (module_name.empty() || value.parent_scope.empty() || value.parent_scope == module_name))
            return value.type;
    }
    return "";
}

static std::optional<TextEdit> declaration_delete_edit(const FileView& file,
                                                       const std::string& module_name,
                                                       const std::string& signal_name) {
    for (const auto& decl : ast_declarations_for_module(file, module_name)) {
        if (decl.name != signal_name || decl.declarator_count != 1)
            continue;

        auto lines = split_lines(file_text(file));
        if (decl.start_line >= 0 && decl.start_line < static_cast<int>(lines.size())) {
            const auto prefix = lines[decl.start_line].substr(0, std::min<size_t>(decl.start_col, lines[decl.start_line].size()));
            if (trim(prefix).empty() && decl.end_line == decl.start_line) {
                return TextEdit{file.uri, decl.start_line, 0, decl.start_line + 1, 0, ""};
            }
        }
        return TextEdit{file.uri, decl.start_line, decl.start_col, decl.end_line, decl.end_col, ""};
    }
    return std::nullopt;
}

static std::optional<TextEdit> replace_or_add_connection(const FileView& file,
                                                         const InstanceEntry& inst,
                                                         const std::string& port,
                                                         const std::string& signal,
                                                         const std::string& old_signal = {},
                                                         bool add_if_missing = true) {
    auto lines = split_lines(file_text(file));
    const int start = std::max(inst.start_line, 0);
    const int end = std::min(inst.end_line, static_cast<int>(lines.size()) - 1);

    for (const auto& conn : inst.connections) {
        if (conn.port_name != port)
            continue;
        if (!old_signal.empty() && conn.signal_name != old_signal)
            continue;
        const int line = conn.line > 0 ? conn.line - 1 : start;
        if (line < 0 || line >= static_cast<int>(lines.size()))
            continue;

        const std::string& old_line = lines[line];
        size_t open = old_line.find('(', static_cast<size_t>(std::max(conn.col, 0)));
        if (open == std::string::npos)
            open = old_line.find('(', static_cast<size_t>(std::max(conn.hint_col - 1, 0)));
        if (open == std::string::npos)
            continue;
        const size_t close = old_line.find(')', open + 1);
        if (close == std::string::npos)
            continue;

        std::string new_line = old_line.substr(0, open + 1) + signal + old_line.substr(close);
        return TextEdit{file.uri, line, 0, line, static_cast<int>(old_line.size()), new_line};
    }

    if (!add_if_missing || lines.empty())
        return std::nullopt;

    // Missing port: insert before the instance-closing ");". This remains a
    // local textual edit because the AST tells us the instance span, but the
    // formatter/user owns the preferred trailing-comma layout.
    int close_line = std::max(0, std::min(end, static_cast<int>(lines.size()) - 1));
    int close_col = static_cast<int>(lines[close_line].size());
    for (int i = close_line; i >= start; --i) {
        auto pos = lines[i].rfind(");");
        if (pos != std::string::npos) {
            close_line = i;
            close_col = static_cast<int>(pos);
            break;
        }
    }
    std::string indent = "    ";
    for (const auto& conn : inst.connections) {
        const int line = conn.line > 0 ? conn.line - 1 : -1;
        if (line >= start && line <= close_line && line < static_cast<int>(lines.size())) {
            const auto dot = lines[line].find('.');
            if (dot != std::string::npos) {
                indent = lines[line].substr(0, dot);
                break;
            }
        }
    }
    return TextEdit{file.uri, close_line, close_col, close_line, close_col,
                    ",\n" + indent + "." + port + "(" + signal + ")"};
}

struct PortSignalEdit {
    std::string port;
    std::string signal;
};

static std::string join_lines(const std::vector<std::string>& lines, int start, int end) {
    std::string out;
    for (int i = start; i <= end; ++i) {
        if (i > start)
            out += '\n';
        out += lines[i];
    }
    out += '\n';
    return out;
}

static std::optional<TextEdit> replace_or_add_connections(const FileView& file,
                                                          const InstanceEntry& inst,
                                                          const std::vector<PortSignalEdit>& desired) {
    if (desired.empty())
        return std::nullopt;

    auto lines = split_lines(file_text(file));
    if (lines.empty())
        return std::nullopt;
    const int start = std::max(inst.start_line, 0);
    const int end = std::min(inst.end_line, static_cast<int>(lines.size()) - 1);
    if (start > end)
        return std::nullopt;

    std::unordered_map<std::string, std::string> desired_by_port;
    std::vector<std::string> desired_order;
    for (const auto& item : desired) {
        if (item.port.empty())
            continue;
        if (!desired_by_port.contains(item.port))
            desired_order.push_back(item.port);
        desired_by_port[item.port] = item.signal;
    }

    std::unordered_set<std::string> handled;
    struct Replacement { size_t begin; size_t end; std::string text; };
    std::unordered_map<int, std::vector<Replacement>> replacements_by_line;

    for (const auto& conn : inst.connections) {
        auto want = desired_by_port.find(conn.port_name);
        if (want == desired_by_port.end())
            continue;

        const int line = conn.line > 0 ? conn.line - 1 : start;
        if (line < start || line > end || line >= static_cast<int>(lines.size()))
            continue;

        const std::string& old_line = lines[line];
        size_t open = old_line.find('(', static_cast<size_t>(std::max(conn.col, 0)));
        if (open == std::string::npos)
            open = old_line.find('(', static_cast<size_t>(std::max(conn.hint_col - 1, 0)));
        if (open == std::string::npos)
            continue;
        const size_t close = old_line.find(')', open + 1);
        if (close == std::string::npos)
            continue;

        replacements_by_line[line].push_back(Replacement{open + 1, close, want->second});
        handled.insert(conn.port_name);
    }

    for (auto& [line, replacements] : replacements_by_line) {
        std::sort(replacements.begin(), replacements.end(),
                  [](const Replacement& a, const Replacement& b) { return a.begin > b.begin; });
        for (const auto& repl : replacements) {
            if (repl.begin > repl.end || repl.end > lines[line].size())
                continue;
            lines[line].replace(repl.begin, repl.end - repl.begin, repl.text);
        }
    }

    std::vector<PortSignalEdit> missing;
    for (const auto& port : desired_order) {
        if (!handled.contains(port))
            missing.push_back(PortSignalEdit{port, desired_by_port[port]});
    }

    if (!missing.empty()) {
        int close_line = end;
        int close_col = static_cast<int>(lines[close_line].size());
        for (int i = end; i >= start; --i) {
            auto pos = lines[i].rfind(");");
            if (pos != std::string::npos) {
                close_line = i;
                close_col = static_cast<int>(pos);
                break;
            }
        }

        std::string indent = "    ";
        for (const auto& conn : inst.connections) {
            const int line = conn.line > 0 ? conn.line - 1 : -1;
            if (line >= start && line <= close_line && line < static_cast<int>(lines.size())) {
                const auto dot = lines[line].find('.');
                if (dot != std::string::npos) {
                    indent = lines[line].substr(0, dot);
                    break;
                }
            }
        }
        if (inst.connections.empty() && close_line >= 0 && close_line < static_cast<int>(lines.size())) {
            const auto first_non_ws = lines[close_line].find_first_not_of(" \t");
            indent = (first_non_ws == std::string::npos ? std::string{} : lines[close_line].substr(0, first_non_ws)) + "    ";
        }
        const auto close_non_ws = lines[close_line].find_first_not_of(" \t");
        const std::string close_indent = close_non_ws == std::string::npos
            ? std::string{}
            : lines[close_line].substr(0, close_non_ws);

        std::string insertion;
        if (!inst.connections.empty())
            insertion += ",";
        insertion += "\n";
        for (size_t i = 0; i < missing.size(); ++i) {
            insertion += indent + "." + missing[i].port + "(" + missing[i].signal + ")";
            if (i + 1 < missing.size())
                insertion += ",";
            insertion += "\n";
        }
        insertion += close_indent;

        if (close_col < 0 || close_col > static_cast<int>(lines[close_line].size()))
            return std::nullopt;
        lines[close_line].insert(static_cast<size_t>(close_col), insertion);
    }

    return TextEdit{file.uri, start, 0, end + 1, 0, join_lines(lines, start, end)};
}

static std::string type_for_decl(std::string type) {
    type = trim(type);
    if (type.empty())
        return "logic";

    // Be tolerant of older callers / cached UI rows that carried only a packed
    // dimension (for example "[5:0]") instead of a complete declaration type.
    // Emitting the raw dimension before the name is not valid SV: declarations
    // need a data object kind or datatype first.  Keep arbitrary dimension text
    // intact so parameterized and macro-based ranges survive:
    //
    //     [DEPTH-1:0]     -> logic [DEPTH-1:0]
    //     [`WIDTH-1:0]    -> logic [`WIDTH-1:0]
    if (!type.empty() && type.front() == '[')
        return "logic " + type;

    // Do not split or rewrite user-defined datatypes.  Package scopes and
    // typedef names (my_pkg::payload_t, payload_t) and syntactic dimensions
    // must remain exactly as indexed / supplied by the UI.
    return type;
}

static std::optional<TextEdit> add_wire_decl(const FileView& file, const std::string& module_name,
                                             const std::string& name, const std::string& type) {
    if (declared_signal(file, module_name, name))
        return std::nullopt;
    const int line = wire_insert_line(file, module_name);
    return TextEdit{file.uri, line, 0, line, 0, type_for_decl(type) + " " + name + ";\n"};
}

struct ModulePortAddition {
    std::string direction;
    std::string type;
    std::string name;
};

static std::vector<TextEdit> add_module_ports(const FileView& file, const std::string& module_name,
                                              const std::vector<ModulePortAddition>& ports) {
    if (ports.empty())
        return {};
    auto mod_it = file.index.module_by_name.find(module_name);
    if (mod_it == file.index.module_by_name.end())
        return {};
    const auto& module = file.index.modules[mod_it->second];
    if (module.header_semi_line < 0 || module.header_semi_col < 0)
        return {};

    auto lines = split_lines(file_text(file));
    std::string indent = "    ";
    const auto& text = file_text(file);
    const int header_start_line = std::max(module.line - 1, 0);
    const int header_start_col = std::max(module.col, 0);

    if (!module.has_port_list) {
        // Module has no port list yet. Replace the complete module header with
        // a complete generated header instead of inserting a raw fragment.
        // Keeping the replacement self-contained also avoids overlapping edits
        // when multiple ports are added to the same module header.
        const size_t header_start = offset_from_position(text, header_start_line, header_start_col);
        const size_t header_end = offset_from_position(text, module.header_semi_line,
                                                       module.header_semi_col + 1);
        if (header_start > header_end || header_end > text.size())
            return {};
        std::string header = text.substr(header_start, header_end - header_start);
        const auto semi = header.rfind(';');
        if (semi == std::string::npos)
            return {};
        std::string generated = "(\n";
        for (size_t i = 0; i < ports.size(); ++i) {
            generated += indent + ports[i].direction + " " + type_for_decl(ports[i].type) +
                         " " + ports[i].name;
            if (i + 1 < ports.size())
                generated += ",";
            generated += "\n";
        }
        generated += ")";
        header.insert(semi, generated);
        return {TextEdit{file.uri, header_start_line, header_start_col,
                         module.header_semi_line, module.header_semi_col + 1,
                         header}};
    }

    if (module.port_list_close_line < 0 || module.port_list_close_col < 0)
        return {};

    const int line = module.port_list_close_line;
    const int col = module.port_list_close_col;
    for (int i = line; i >= 0; --i) {
        if (i >= static_cast<int>(lines.size()))
            continue;
        if (lines[i].find("input") != std::string::npos || lines[i].find("output") != std::string::npos ||
            lines[i].find("inout") != std::string::npos) {
            indent = lines[i].substr(0, lines[i].find_first_not_of(" \t"));
            break;
        }
    }

    auto replace_complete_header = [&](const std::string& insertion) -> std::vector<TextEdit> {
        const size_t header_start = offset_from_position(text, header_start_line, header_start_col);
        const size_t header_end = offset_from_position(text, module.header_semi_line,
                                                       module.header_semi_col + 1);
        const size_t close = offset_from_position(text, line, col);
        if (header_start > header_end || close < header_start || close > header_end ||
            header_end > text.size())
            return {};
        std::string header = text.substr(header_start, header_end - header_start);
        header.insert(close - header_start, insertion);
        return {TextEdit{file.uri, header_start_line, header_start_col,
                         module.header_semi_line, module.header_semi_col + 1,
                         header}};
    };

    if (module.ansi_port_list || !module.port_list_has_ports) {
        // Existing ANSI style: keep direction/type in the module header.  The
        // whole header replacement is still "generated code" for formatting
        // purposes because Connect constructs this replacement text; unrelated
        // module body/source remains untouched.
        const std::string prefix = module.port_list_has_ports ? ",\n" : "\n";
        std::string generated = prefix;
        for (size_t i = 0; i < ports.size(); ++i) {
            generated += indent + ports[i].direction + " " + type_for_decl(ports[i].type) +
                         " " + ports[i].name;
            if (i + 1 < ports.size())
                generated += ",\n";
        }
        return replace_complete_header(generated);
    }

    // Existing non-ANSI style: add only the bare name to the header list, then
    // add a separate port declaration after the module header semicolon.
    std::string name_indent = indent;
    for (int i = line; i >= 0; --i) {
        if (i >= static_cast<int>(lines.size()))
            continue;
        const auto first = lines[i].find_first_not_of(" \t");
        if (first != std::string::npos && lines[i].find("module") == std::string::npos) {
            name_indent = lines[i].substr(0, first);
            break;
        }
    }
    std::string header_names = ",\n";
    for (size_t i = 0; i < ports.size(); ++i) {
        header_names += name_indent + ports[i].name;
        if (i + 1 < ports.size())
            header_names += ",\n";
    }
    auto edits = replace_complete_header(header_names);
    if (edits.empty())
        return {};
    std::string declarations;
    for (const auto& port : ports)
        declarations += port.direction + " " + type_for_decl(port.type) + " " + port.name + ";\n";
    edits.push_back(TextEdit{file.uri, module.header_semi_line + 1, 0,
                             module.header_semi_line + 1, 0, declarations});
    return edits;
}

static std::vector<TextEdit> add_module_port(const FileView& file, const std::string& module_name,
                                             const std::string& direction,
                                             const std::string& type,
                                             const std::string& port_name) {
    return add_module_ports(file, module_name, {{direction, type, port_name}});
}

static std::string workspace_edit_json(const std::vector<TextEdit>& edits) {
    if (edits.empty())
        return "null";
    std::map<std::string, std::vector<TextEdit>> by_uri;
    for (const auto& e : edits)
        by_uri[e.uri].push_back(e);
    std::string out = "{\"changes\":{";
    bool first_uri = true;
    for (const auto& [uri, uri_edits] : by_uri) {
        if (!first_uri)
            out += ",";
        first_uri = false;
        out += q(uri) + ":[";
        for (size_t i = 0; i < uri_edits.size(); ++i) {
            const auto& e = uri_edits[i];
            if (i)
                out += ",";
            out += "{\"range\":{\"start\":{\"line\":" + std::to_string(e.sl) +
                   ",\"character\":" + std::to_string(e.sc) + "},\"end\":{\"line\":" +
                   std::to_string(e.el) + ",\"character\":" + std::to_string(e.ec) +
                   "}},\"newText\":" + q(e.text) + "}";
        }
        out += "]";
    }
    out += "}}";
    return out;
}

static std::string error_json(const std::string& msg) { return "{\"error\":" + q(msg) + "}"; }

static std::string preview_json(const std::string& wire_name, const std::string& wire_type,
                                const std::string& lca_module,
                                const std::vector<PreviewEdit>& edits,
                                const std::vector<std::string>& warnings) {
    std::string out = "{\"wire_name\":" + q(wire_name) + ",\"wire_type\":" + q(wire_type) +
                      ",\"lca_module\":" + q(lca_module) + ",\"edits\":[";
    for (size_t i = 0; i < edits.size(); ++i) {
        if (i)
            out += ",";
        out += "{\"file\":" + q(edits[i].file) + ",\"line\":" + std::to_string(edits[i].line) +
               ",\"description\":" + q(edits[i].description) +
               ",\"is_warning\":" + (edits[i].warning ? "true" : "false") + "}";
    }
    out += "],\"warnings\":[";
    for (size_t i = 0; i < warnings.size(); ++i) {
        if (i)
            out += ",";
        out += q(warnings[i]);
    }
    out += "]}";
    return out;
}

static std::vector<std::string> path_pairs_to_lca(std::string path, const std::string& lca) {
    std::vector<std::string> children;
    while (path != lca && path.find('.') != std::string::npos) {
        children.push_back(path);
        path = path.substr(0, path.find_last_of('.'));
    }
    return children;
}

static std::string lca_path(std::string a, std::string b) {
    if (a.find('.') == std::string::npos || b.find('.') == std::string::npos)
        return {};
    a = a.substr(0, a.find_last_of('.'));
    b = b.substr(0, b.find_last_of('.'));
    std::vector<std::string> ap, bp;
    std::stringstream as(a), bs(b);
    std::string part;
    while (std::getline(as, part, '.')) ap.push_back(part);
    while (std::getline(bs, part, '.')) bp.push_back(part);
    std::string out;
    for (size_t i = 0; i < std::min(ap.size(), bp.size()) && ap[i] == bp[i]; ++i) {
        if (!out.empty())
            out += ".";
        out += ap[i];
    }
    return out;
}

struct ConnectBuildResult {
    std::string error;
    std::string wire_type;
    std::string lca_module;
    std::vector<TextEdit> edits;
    std::vector<PreviewEdit> preview;
    std::vector<std::string> warnings;
};

static ConnectBuildResult build_connect(const Analyzer& analyzer, const std::string& uri,
                                        const std::string& source_path,
                                        const std::string& source_port,
                                        const std::string& dest_path,
                                        const std::string& dest_port,
                                        const std::string& wire_name,
                                        const std::vector<std::string>& source_boundary_ports = {},
                                        const std::vector<std::string>& dest_boundary_ports = {}) {
    ConnectBuildResult r;
    auto files = collect_files(analyzer, uri);
    const auto lookup = build_design_lookup(files);

    // Preview/apply only needs the two requested instance routes.  Avoid
    // expanding every possible hierarchical path in the project; resolving
    // `top.u_a.u_leaf` is just a step-by-step walk through indexed instances:
    // root module -> child instance -> child module -> ... .
    auto source_route = resolve_instance_route(lookup, source_path, r.error);
    if (!source_route)
        return r;
    auto dest_route = resolve_instance_route(lookup, dest_path, r.error);
    if (!dest_route)
        return r;
    auto hierarchy = route_hierarchy_map(*source_route, *dest_route);

    const auto& src = hierarchy.at(source_path);
    const auto& dst = hierarchy.at(dest_path);
    auto sp = port_on_module(files, src.module_name, source_port);
    auto dp = port_on_module(files, dst.module_name, dest_port);
    if (sp && sp->direction != "output") {
        r.error = "port '" + source_port + "' is not an output port";
        return r;
    }
    if (dp && dp->direction != "input") {
        r.error = "port '" + dest_port + "' is not an input port";
        return r;
    }

    // Leaf ports can be typed as new names from the UI.  Existing source
    // output type is best; otherwise use an existing destination input type if
    // present, and fall back to a scalar logic declaration for entirely new
    // leaf-to-leaf connections.
    if (sp)
        r.wire_type = signal_decl_type_for_port(*sp).empty() ? "logic" : signal_decl_type_for_port(*sp);
    else if (dp)
        r.wire_type = signal_decl_type_for_port(*dp).empty() ? "logic" : signal_decl_type_for_port(*dp);
    else
        r.wire_type = "logic";

    if (sp && dp && decl_type_for_port(*sp) != decl_type_for_port(*dp))
        r.warnings.push_back("type mismatch: source port '" + decl_type_for_port(*sp) + "' vs dest port '" + decl_type_for_port(*dp) + "' — using source type");
    const std::string lca = lca_path(source_path, dest_path);
    if (lca.empty()) { r.error = "no common ancestor found"; return r; }
    const auto root_mod = lca.substr(lca.find_last_of('.') == std::string::npos ? 0 : lca.find_last_of('.') + 1);
    r.lca_module = hierarchy.contains(lca) ? hierarchy.at(lca).module_name : root_mod;

    auto source_steps = path_pairs_to_lca(source_path, lca);
    auto dest_steps = path_pairs_to_lca(dest_path, lca);
    const size_t needed_source_ports = source_steps.empty() ? 0 : source_steps.size() - 1;
    const size_t needed_dest_ports = dest_steps.empty() ? 0 : dest_steps.size() - 1;
    if (source_boundary_ports.size() < needed_source_ports) {
        r.error = "missing source hierarchy port choices: expected " +
                  std::to_string(needed_source_ports) + ", got " +
                  std::to_string(source_boundary_ports.size());
        return r;
    }
    if (dest_boundary_ports.size() < needed_dest_ports) {
        r.error = "missing destination hierarchy port choices: expected " +
                  std::to_string(needed_dest_ports) + ", got " +
                  std::to_string(dest_boundary_ports.size());
        return r;
    }

    // If the user routes through existing hierarchy boundary ports, size the
    // LCA bridge wire from those boundary ports rather than from the leaf cell
    // pin.  In the demo case `inv.o` is 1 bit, but `memory.o_data` is the real
    // exported bus that should determine the top-level bridge declaration.
    if (!source_boundary_ports.empty()) {
        const auto& child = hierarchy.at(source_steps.front());
        if (auto bp = port_on_module(files, child.parent_module, source_boundary_ports.front())) {
            const auto typ = signal_decl_type_for_port(*bp);
            if (!typ.empty())
                r.wire_type = typ;
        }
    } else if (!dest_boundary_ports.empty()) {
        const auto& child = hierarchy.at(dest_steps.front());
        if (auto bp = port_on_module(files, child.parent_module, dest_boundary_ports.front())) {
            const auto typ = signal_decl_type_for_port(*bp);
            if (!typ.empty())
                r.wire_type = typ;
        }
    }

    auto file_by_uri = std::unordered_map<std::string, const FileView*>();
    for (const auto& f : files)
        file_by_uri.emplace(f.uri, &f);

    struct InstanceEditGroup {
        const FileView* file{nullptr};
        InstanceEntry inst;
        std::vector<PortSignalEdit> desired;
    };
    std::vector<InstanceEditGroup> instance_edit_groups;
    std::unordered_map<std::string, size_t> instance_group_by_key;

    struct PendingModulePortEdits {
        const FileView* file{nullptr};
        std::string module_name;
        std::vector<ModulePortAddition> ports;
    };
    std::vector<PendingModulePortEdits> pending_module_ports;
    std::unordered_map<std::string, size_t> pending_module_port_by_key;

    auto instance_group_key = [](const ResolvedInst& inst) {
        return inst.file_uri + "|" + inst.parent_module + "|" + inst.inst_name + "|" +
               std::to_string(inst.entry.start_line) + "|" + std::to_string(inst.entry.end_line);
    };

    auto add_step = [&](const ResolvedInst& inst, const std::string& port,
                        const std::string& signal, bool warn_override) {
        const auto* f = file_by_uri[inst.file_uri];
        auto old = connection_for(inst.entry, port);
        if (old && !old->signal_name.empty() && warn_override)
            r.warnings.push_back(inst.path + "." + port + " was connected to '" + old->signal_name + "' — will override");

        const auto key = instance_group_key(inst);
        auto [it, inserted] = instance_group_by_key.emplace(key, instance_edit_groups.size());
        if (inserted)
            instance_edit_groups.push_back(InstanceEditGroup{.file = f, .inst = inst.entry});
        instance_edit_groups[it->second].desired.push_back(PortSignalEdit{port, signal});

        r.preview.push_back(PreviewEdit{basename_from_uri(f->uri), inst.entry.start_line + 1,
                                        "connect " + inst.path + "." + port + "(" + signal + ")" +
                                            (old && !old->signal_name.empty() ? " [overrides " + old->signal_name + "]" : ""),
                                        old && !old->signal_name.empty()});
    };

    auto queue_module_port = [&](const FileView* module_file, const std::string& module_name,
                                 const std::string& direction, const std::string& type,
                                 const std::string& port_name, const std::string& preview_description) {
        if (!module_file)
            return;
        const std::string key = module_file->uri + "|" + module_name;
        auto [it, inserted] = pending_module_port_by_key.emplace(key, pending_module_ports.size());
        if (inserted)
            pending_module_ports.push_back(PendingModulePortEdits{.file = module_file,
                                                                  .module_name = module_name});
        auto& pending = pending_module_ports[it->second];
        for (const auto& existing : pending.ports) {
            if (existing.name == port_name)
                return;
        }
        pending.ports.push_back(ModulePortAddition{direction, type, port_name});
        r.preview.push_back(PreviewEdit{basename_from_uri(module_file->uri), 0,
                                        preview_description, false});
    };

    auto add_boundary_port = [&](const ResolvedInst& child_inst, const std::string& direction,
                                 const std::string& port_name) {
        const FileView* module_file = nullptr;
        const auto* module = find_module(files, child_inst.parent_module, &module_file);
        if (!module_file)
            return;
        if (module && module->port_by_name.contains(port_name))
            return;
        queue_module_port(module_file, child_inst.parent_module, direction, r.wire_type, port_name,
                          "add " + direction + " " + r.wire_type + " " + port_name +
                              " to " + child_inst.parent_module);
    };

    auto add_leaf_port = [&](const ResolvedInst& leaf_inst, const std::string& leaf_module,
                             const std::string& direction, const std::string& port_name) {
        const FileView* module_file = nullptr;
        const auto* module = find_module(files, leaf_module, &module_file);
        if (!module_file)
            return;
        if (module && module->port_by_name.contains(port_name))
            return;
        queue_module_port(module_file, leaf_module, direction, r.wire_type, port_name,
                          "add " + direction + " " + r.wire_type + " " + port_name +
                              " to " + leaf_module + " for " + leaf_inst.path);
    };

    if (!sp)
        add_leaf_port(src, src.module_name, "output", source_port);
    if (!dp)
        add_leaf_port(dst, dst.module_name, "input", dest_port);

    // Source side: export the selected leaf output upward through each ancestor
    // module port chosen by the user.  The final child-of-LCA instance connects
    // to the LCA-local bridge wire.
    for (size_t i = 0; i < source_steps.size(); ++i) {
        const auto& inst = hierarchy.at(source_steps[i]);
        const std::string inst_port = (i == 0) ? source_port : source_boundary_ports[i - 1];
        const std::string signal = (i + 1 < source_steps.size()) ? source_boundary_ports[i]
                                                                 : wire_name;
        add_step(inst, inst_port, signal, i == 0);
        if (i + 1 < source_steps.size())
            add_boundary_port(inst, "output", source_boundary_ports[i]);
    }

    // Destination side: import the LCA-local bridge wire downward through each
    // user-chosen ancestor input port until the selected leaf input is driven.
    for (size_t i = 0; i < dest_steps.size(); ++i) {
        const auto& inst = hierarchy.at(dest_steps[i]);
        const std::string inst_port = (i == 0) ? dest_port : dest_boundary_ports[i - 1];
        const std::string signal = (i + 1 < dest_steps.size()) ? dest_boundary_ports[i]
                                                               : wire_name;
        add_step(inst, inst_port, signal, i == 0);
        if (i + 1 < dest_steps.size())
            add_boundary_port(inst, "input", dest_boundary_ports[i]);
    }

    for (const auto& pending : pending_module_ports) {
        if (!pending.file)
            continue;
        auto edits = add_module_ports(*pending.file, pending.module_name, pending.ports);
        r.edits.insert(r.edits.end(), edits.begin(), edits.end());
    }

    for (const auto& group : instance_edit_groups) {
        if (!group.file)
            continue;
        if (auto edit = replace_or_add_connections(*group.file, group.inst, group.desired))
            r.edits.push_back(*edit);
    }

    const FileView* lca_file = nullptr;
    find_module(files, r.lca_module, &lca_file);
    if (lca_file) {
        if (auto edit = add_wire_decl(*lca_file, r.lca_module, wire_name, r.wire_type)) {
            r.preview.push_back(PreviewEdit{basename_from_uri(lca_file->uri), edit->sl + 1,
                                            "declare " + r.wire_type + " " + wire_name + " in " + r.lca_module, false});
            r.edits.push_back(*edit);
        }
    }
    return r;
}

static std::string module_ports_json(const ModuleEntry& module, bool type_key_str = false) {
    std::string out = "[";
    for (size_t i = 0; i < module.ports.size(); ++i) {
        if (i)
            out += ",";
        const auto& p = module.ports[i];
        out += "{\"name\":" + q(p.name) + ",\"direction\":" + q(p.direction) +
               (type_key_str ? ",\"type_str\":" : ",\"type\":") + q(decl_type_for_port(p)) + "}";
    }
    out += "]";
    return out;
}

static std::optional<std::pair<const FileView*, const InstanceEntry*>>
find_current_file_instance(const std::vector<FileView>& files, const std::string& uri,
                           const std::string& inst_name) {
    for (const auto& file : files) {
        if (file.uri != uri)
            continue;
        for (const auto& inst : file.index.instances) {
            if (inst.instance_name == inst_name)
                return std::make_pair(&file, &inst);
        }
    }
    for (const auto& file : files) {
        for (const auto& inst : file.index.instances) {
            if (inst.instance_name == inst_name)
                return std::make_pair(&file, &inst);
        }
    }
    return std::nullopt;
}


} // namespace

std::string connect_info_json(const Analyzer& analyzer, const std::string& uri) {
    auto files = collect_files(analyzer, uri);
    auto hierarchy = build_hierarchy(files);
    std::string out = "{\"modules\":{";
    bool first_mod = true;
    for (const auto& file : files) {
        for (const auto& module : file.index.modules) {
            if (!first_mod)
                out += ",";
            first_mod = false;
            out += q(module.name) + ":{\"ports\":" + module_ports_json(module, true) + ",\"instances\":[";
            bool first_inst = true;
            for (const auto& [path, inst] : hierarchy) {
                if (inst.module_name != module.name)
                    continue;
                if (!first_inst)
                    out += ",";
                first_inst = false;
                out += "{\"inst_name\":" + q(inst.inst_name) + ",\"hierarchical_path\":" + q(path) +
                       ",\"file_uri\":" + q(inst.file_uri) + "}";
            }
            out += "]}";
        }
    }
    out += "}}";
    return out;
}

std::string connect_apply_preview_json(const Analyzer& analyzer, const std::string& uri,
                                       const std::string& source_path,
                                       const std::string& source_port,
                                       const std::string& dest_path,
                                       const std::string& dest_port,
                                       const std::string& wire_name,
                                       const std::vector<std::string>& source_boundary_ports,
                                       const std::vector<std::string>& dest_boundary_ports) {
    auto r = build_connect(analyzer, uri, source_path, source_port, dest_path, dest_port,
                           wire_name, source_boundary_ports, dest_boundary_ports);
    if (!r.error.empty())
        return error_json(r.error);
    return preview_json(wire_name, r.wire_type, r.lca_module, r.preview, r.warnings);
}

std::string connect_apply_edit_json(const Analyzer& analyzer, const std::string& uri,
                                    const std::string& source_path,
                                    const std::string& source_port,
                                    const std::string& dest_path,
                                    const std::string& dest_port,
                                    const std::string& wire_name,
                                    const std::vector<std::string>& source_boundary_ports,
                                    const std::vector<std::string>& dest_boundary_ports) {
    auto r = build_connect(analyzer, uri, source_path, source_port, dest_path, dest_port,
                           wire_name, source_boundary_ports, dest_boundary_ports);
    if (!r.error.empty())
        return error_json(r.error);
    return workspace_edit_json(r.edits);
}

std::string interface_json(const Analyzer& analyzer, const std::string& uri,
                           const std::string& inst1_name, const std::string& inst2_name) {
    auto files = collect_files(analyzer, uri);
    auto a = find_current_file_instance(files, uri, inst1_name);
    auto b = find_current_file_instance(files, uri, inst2_name);
    if (!a || !b)
        return error_json("instance not found");
    const auto& inst1 = *a->second;
    const auto& inst2 = *b->second;
    const auto* mod1 = find_module(files, inst1.module_name);
    const auto* mod2 = find_module(files, inst2.module_name);
    if (!mod1 || !mod2)
        return error_json("module not found");

    std::map<std::string, NamedPortConn> c2;
    for (const auto& c : inst2.connections)
        c2[c.signal_name] = c;

    std::string out = "{\"inst1\":{\"name\":" + q(inst1_name) + ",\"ports\":" + module_ports_json(*mod1) +
                      "},\"inst2\":{\"name\":" + q(inst2_name) + ",\"ports\":" + module_ports_json(*mod2) +
                      "},\"connections\":[";
    bool first = true;
    for (const auto& c1 : inst1.connections) {
        auto it = c2.find(c1.signal_name);
        if (c1.signal_name.empty() || it == c2.end()) {
            if (!first) out += ",";
            first = false;
            out += "{\"inst1_port\":" + q(c1.port_name) + ",\"inst2_port\":\"\",\"signal\":" + q(c1.signal_name) +
                   ",\"signal_type\":" + q(signal_type_in_file(*a->first, inst1.parent_module, c1.signal_name)) + "}";
            continue;
        }
        if (!first) out += ",";
        first = false;
        out += "{\"inst1_port\":" + q(c1.port_name) + ",\"inst2_port\":" + q(it->second.port_name) +
               ",\"signal\":" + q(c1.signal_name) + ",\"signal_type\":" + q(signal_type_in_file(*a->first, inst1.parent_module, c1.signal_name)) + "}";
    }
    out += "]}";
    return out;
}

std::string single_interface_json(const Analyzer& analyzer, const std::string& uri,
                                  const std::string& inst_name) {
    auto files = collect_files(analyzer, uri);
    auto target = find_current_file_instance(files, uri, inst_name);
    if (!target)
        return error_json("instance '" + inst_name + "' not found");
    const auto& file = *target->first;
    const auto& inst = *target->second;
    const auto* mod = find_module(files, inst.module_name);
    if (!mod)
        return error_json("module not found");

    std::map<std::string, NamedPortConn> self_conn;
    for (const auto& c : inst.connections)
        self_conn[c.port_name] = c;

    std::multimap<std::string, std::tuple<std::string, std::string, std::string, std::string>> others;
    for (const auto& other : file.index.instances) {
        if (other.instance_name == inst.instance_name || other.parent_module != inst.parent_module)
            continue;
        const auto* omod = find_module(files, other.module_name);
        for (const auto& c : other.connections) {
            std::string dir, typ;
            if (omod) {
                auto pit = omod->port_by_name.find(c.port_name);
                if (pit != omod->port_by_name.end()) {
                    dir = omod->ports[pit->second].direction;
                    typ = decl_type_for_port(omod->ports[pit->second]);
                }
            }
            others.emplace(c.signal_name, std::make_tuple(other.instance_name, c.port_name, dir, typ));
        }
    }

    std::string out = "{\"inst\":{\"name\":" + q(inst_name) + "},\"rows\":[";
    bool first = true;
    for (const auto& p : mod->ports) {
        const auto sig = self_conn.contains(p.name) ? self_conn[p.name].signal_name : std::string{};
        auto range = others.equal_range(sig);
        bool any = sig.empty() ? false : range.first != range.second;
        if (!any) {
            if (!first) out += ","; first = false;
            out += "{\"port_name\":" + q(p.name) + ",\"port_type\":" + q(decl_type_for_port(p)) + ",\"port_dir\":" + q(p.direction) +
                   ",\"signal\":" + q(sig) + ",\"signal_type\":" + q(signal_type_in_file(file, inst.parent_module, sig)) +
                   ",\"other_inst\":\"\",\"other_port\":\"\",\"other_dir\":\"\",\"other_type\":\"\"}";
        } else {
            for (auto it = range.first; it != range.second; ++it) {
                const auto& [oi, op, od, ot] = it->second;
                if (!first) out += ","; first = false;
                out += "{\"port_name\":" + q(p.name) + ",\"port_type\":" + q(decl_type_for_port(p)) + ",\"port_dir\":" + q(p.direction) +
                       ",\"signal\":" + q(sig) + ",\"signal_type\":" + q(signal_type_in_file(file, inst.parent_module, sig)) +
                       ",\"other_inst\":" + q(oi) + ",\"other_port\":" + q(op) + ",\"other_dir\":" + q(od) +
                       ",\"other_type\":" + q(ot) + "}";
            }
        }
    }
    out += "]}";
    return out;
}

std::string interface_connect_edit_json(const Analyzer& analyzer, const std::string& uri,
                                        const std::string& inst1_name,
                                        const std::string& inst2_name,
                                        const std::string& inst1_port,
                                        const std::string& inst2_port,
                                        const std::string& wire_name,
                                        const std::string& wire_type) {
    auto files = collect_files(analyzer, uri);
    auto a = find_current_file_instance(files, uri, inst1_name);
    auto b = find_current_file_instance(files, uri, inst2_name);
    if (!a || !b)
        return "null";

    // The UI sends a wire_type string from the selected row, but the row can
    // represent either side of the connection.  Declarations must follow the
    // driving side, so derive the declaration datatype from the selected output
    // port rather than trusting the caller-provided type.  If port metadata is
    // unavailable, keep the caller type as a compatibility fallback.
    std::string declaration_type = wire_type;
    const auto* mod1 = find_module(files, a->second->module_name);
    const auto* mod2 = find_module(files, b->second->module_name);
    auto choose_output_type = [&](const ModuleEntry* module, const std::string& port_name) {
        if (!module)
            return;
        const auto it = module->port_by_name.find(port_name);
        if (it == module->port_by_name.end())
            return;
        const auto& port = module->ports[it->second];
        if (port.direction == "output")
            declaration_type = signal_decl_type_for_port(port);
    };
    choose_output_type(mod1, inst1_port);
    choose_output_type(mod2, inst2_port);

    std::vector<TextEdit> edits;
    if (auto e = replace_or_add_connection(*a->first, *a->second, inst1_port, wire_name)) edits.push_back(*e);
    if (auto e = replace_or_add_connection(*b->first, *b->second, inst2_port, wire_name)) edits.push_back(*e);
    if (auto e = add_wire_decl(*a->first, a->second->parent_module, wire_name, declaration_type)) edits.push_back(*e);
    return workspace_edit_json(edits);
}

std::string interface_disconnect_edit_json(const Analyzer& analyzer, const std::string& uri,
                                           const std::string& inst1_name,
                                           const std::string& inst2_name,
                                           const std::string& inst1_port,
                                           const std::string& inst2_port,
                                           const std::string& signal_name) {
    auto files = collect_files(analyzer, uri);
    auto a = find_current_file_instance(files, uri, inst1_name);
    auto b = find_current_file_instance(files, uri, inst2_name);
    if (!a || !b)
        return "null";
    std::vector<TextEdit> edits;
    if (!inst1_port.empty()) if (auto e = replace_or_add_connection(*a->first, *a->second, inst1_port, "", signal_name, false)) edits.push_back(*e);
    if (!inst2_port.empty()) if (auto e = replace_or_add_connection(*b->first, *b->second, inst2_port, "", signal_name, false)) edits.push_back(*e);

    if (auto e = declaration_delete_edit(*a->first, a->second->parent_module, signal_name))
        edits.push_back(*e);
    return workspace_edit_json(edits);
}
