#include "connect.hpp"
#include "../dynamic_file_index.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace {

struct FileView {
    std::string uri;
    std::string text;
    SyntaxIndex index;
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
        files.push_back(FileView{state_uri, state->text, build_current_ast_structural_index(*state)});
    });

    // Filelist entries fill in library modules / sibling modules. If an extra
    // file is also open, skip it so the open-buffer version wins.
    for (const auto& extra : analyzer.extra_index_snapshots()) {
        if (!seen.insert(extra.uri).second)
            continue;
        files.push_back(FileView{extra.uri, {}, extra.index});
    }

    // Be defensive for command calls that arrive before didOpen is processed.
    if (!seen.contains(uri)) {
        if (auto state = analyzer.get_state(uri))
            files.push_back(FileView{uri, state->text, build_current_ast_structural_index(*state)});
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

static int line_count_before(const std::string& s, size_t off) {
    return static_cast<int>(std::count(s.begin(), s.begin() + std::min(off, s.size()), '\n'));
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

static std::optional<TextEdit> replace_or_add_connection(const FileView& file,
                                                         const InstanceEntry& inst,
                                                         const std::string& port,
                                                         const std::string& signal,
                                                         const std::string& old_signal = {},
                                                         bool add_if_missing = true) {
    auto lines = split_lines(file.text);
    const int start = std::max(inst.start_line, 0);
    const int end = std::min(inst.end_line, static_cast<int>(lines.size()) - 1);
    const std::regex conn_re(R"(\.\s*(\w+)\s*\(([^)]*)\))");

    for (int i = start; i <= end; ++i) {
        const std::string old_line = lines[i];
        std::string new_line;
        std::sregex_iterator it(old_line.begin(), old_line.end(), conn_re), last;
        size_t cursor = 0;
        bool changed = false;
        for (; it != last; ++it) {
            const auto& m = *it;
            new_line.append(old_line, cursor, static_cast<size_t>(m.position()) - cursor);
            if (m[1].str() == port && (old_signal.empty() || trim(m[2].str()) == old_signal)) {
                new_line += "." + port + "(" + signal + ")";
                changed = true;
            } else {
                new_line += m.str();
            }
            cursor = static_cast<size_t>(m.position() + m.length());
        }
        new_line.append(old_line, cursor, std::string::npos);
        if (changed)
            return TextEdit{file.uri, i, 0, i, static_cast<int>(old_line.size()), new_line};
    }

    if (!add_if_missing)
        return std::nullopt;

    // Missing port: insert before the instance-closing ");". This mirrors the
    // Python command's intentionally simple textual behavior and keeps the
    // edit local to the instance instead of regenerating the whole block.
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
    for (int i = end; i >= start; --i) {
        std::smatch m;
        if (std::regex_search(lines[i], m, std::regex(R"(^(\s*)\.)"))) {
            indent = m[1].str();
            break;
        }
    }
    return TextEdit{file.uri, close_line, close_col, close_line, close_col,
                    ",\n" + indent + "." + port + "(" + signal + ")"};
}

static bool declared_signal(const std::string& text, const std::string& name) {
    const std::regex decl_re("\\b(?:wire|logic|reg)\\b[^;]*\\b" + name + "\\b");
    return std::regex_search(text, decl_re);
}

static int wire_insert_line(const std::string& text) {
    auto lines = split_lines(text);
    int last_decl = -1;
    for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
        if (std::regex_search(lines[i], std::regex(R"(^\s*(?:input|output|inout|wire|logic|reg)\b)")))
            last_decl = i;
    }
    if (last_decl >= 0)
        return last_decl + 1;
    for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
        if (lines[i].find(");") != std::string::npos)
            return i + 1;
    }
    return 0;
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

static std::optional<TextEdit> add_wire_decl(const FileView& file, const std::string& name,
                                             const std::string& type) {
    if (declared_signal(file.text, name))
        return std::nullopt;
    const int line = wire_insert_line(file.text);
    return TextEdit{file.uri, line, 0, line, 0, type_for_decl(type) + " " + name + ";\n"};
}

static std::optional<TextEdit> add_module_port(const FileView& file, const std::string& module_name,
                                               const std::string& direction,
                                               const std::string& type,
                                               const std::string& port_name) {
    const auto module_pos = file.text.find("module " + module_name);
    if (module_pos == std::string::npos)
        return std::nullopt;
    const auto semi = file.text.find(';', module_pos);
    if (semi == std::string::npos)
        return std::nullopt;
    const auto close = file.text.rfind(')', semi);
    if (close == std::string::npos || close < module_pos)
        return std::nullopt;
    auto [line, col] = offset_to_pos(file.text, close);
    auto lines = split_lines(file.text);
    std::string indent = "    ";
    for (int i = line; i >= 0; --i) {
        if (lines[i].find("input") != std::string::npos || lines[i].find("output") != std::string::npos ||
            lines[i].find("inout") != std::string::npos) {
            indent = lines[i].substr(0, lines[i].find_first_not_of(" \t"));
            break;
        }
    }
    return TextEdit{file.uri, line, col, line, col,
                    ",\n" + indent + direction + " " + type_for_decl(type) + " " + port_name};
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
                                        const std::string& wire_name) {
    ConnectBuildResult r;
    auto files = collect_files(analyzer, uri);
    auto hierarchy = build_hierarchy(files);
    if (!hierarchy.contains(source_path)) {
        r.error = "instance '" + source_path + "' not found";
        return r;
    }
    if (!hierarchy.contains(dest_path)) {
        r.error = "instance '" + dest_path + "' not found";
        return r;
    }
    const auto& src = hierarchy.at(source_path);
    const auto& dst = hierarchy.at(dest_path);
    auto sp = port_on_module(files, src.module_name, source_port);
    auto dp = port_on_module(files, dst.module_name, dest_port);
    if (!sp) { r.error = "port '" + source_port + "' not found"; return r; }
    if (!dp) { r.error = "port '" + dest_port + "' not found"; return r; }
    if (sp->direction != "output") { r.error = "port '" + source_port + "' is not an output port"; return r; }
    if (dp->direction != "input") { r.error = "port '" + dest_port + "' is not an input port"; return r; }
    r.wire_type = signal_decl_type_for_port(*sp).empty() ? "logic" : signal_decl_type_for_port(*sp);
    if (decl_type_for_port(*sp) != decl_type_for_port(*dp))
        r.warnings.push_back("type mismatch: source port '" + decl_type_for_port(*sp) + "' vs dest port '" + decl_type_for_port(*dp) + "' — using source type");
    const std::string lca = lca_path(source_path, dest_path);
    if (lca.empty()) { r.error = "no common ancestor found"; return r; }
    const auto root_mod = lca.substr(lca.find_last_of('.') == std::string::npos ? 0 : lca.find_last_of('.') + 1);
    r.lca_module = hierarchy.contains(lca) ? hierarchy.at(lca).module_name : root_mod;

    auto file_by_uri = std::unordered_map<std::string, const FileView*>();
    for (const auto& f : files)
        file_by_uri.emplace(f.uri, &f);

    auto add_step = [&](const ResolvedInst& inst, const std::string& port, bool warn_override) {
        const auto* f = file_by_uri[inst.file_uri];
        auto old = connection_for(inst.entry, port);
        if (old && !old->signal_name.empty() && warn_override)
            r.warnings.push_back(inst.path + "." + port + " was connected to '" + old->signal_name + "' — will override");
        if (auto edit = replace_or_add_connection(*f, inst.entry, port, wire_name)) {
            r.preview.push_back(PreviewEdit{basename_from_uri(f->uri), inst.entry.start_line + 1,
                                            "connect " + inst.inst_name + "." + port + "(" + wire_name + ")" +
                                                (old && !old->signal_name.empty() ? " [overrides " + old->signal_name + "]" : ""),
                                            old && !old->signal_name.empty()});
            r.edits.push_back(*edit);
        }
    };

    for (const auto& child_path : path_pairs_to_lca(source_path, lca)) {
        const auto& inst = hierarchy.at(child_path);
        add_step(inst, child_path == source_path ? source_port : wire_name, child_path == source_path);
        if (inst.parent_path != lca && hierarchy.contains(inst.parent_path)) {
            const auto& parent = hierarchy.at(inst.parent_path);
            const FileView* mf = nullptr;
            find_module(files, parent.module_name, &mf);
            if (mf) {
                if (auto edit = add_module_port(*mf, parent.module_name, "output", r.wire_type, wire_name)) {
                    r.preview.push_back(PreviewEdit{basename_from_uri(mf->uri), edit->sl + 1,
                                                    "add output " + r.wire_type + " " + wire_name + " to " + parent.module_name, false});
                    r.edits.push_back(*edit);
                }
            }
        }
    }
    for (const auto& child_path : path_pairs_to_lca(dest_path, lca)) {
        const auto& inst = hierarchy.at(child_path);
        add_step(inst, child_path == dest_path ? dest_port : wire_name, child_path == dest_path);
        if (inst.parent_path != lca && hierarchy.contains(inst.parent_path)) {
            const auto& parent = hierarchy.at(inst.parent_path);
            const FileView* mf = nullptr;
            find_module(files, parent.module_name, &mf);
            if (mf) {
                if (auto edit = add_module_port(*mf, parent.module_name, "input", r.wire_type, wire_name)) {
                    r.preview.push_back(PreviewEdit{basename_from_uri(mf->uri), edit->sl + 1,
                                                    "add input " + r.wire_type + " " + wire_name + " to " + parent.module_name, false});
                    r.edits.push_back(*edit);
                }
            }
        }
    }

    const FileView* lca_file = nullptr;
    find_module(files, r.lca_module, &lca_file);
    if (lca_file) {
        if (auto edit = add_wire_decl(*lca_file, wire_name, r.wire_type)) {
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

static std::string signal_type_in_text(const std::string& text, const std::string& sig) {
    if (sig.empty())
        return "";
    const std::regex re("\\b(?:wire|logic|reg)\\b([^;]*?)\\b" + sig + "\\b");
    std::smatch m;
    if (std::regex_search(text, m, re)) {
        std::string s = trim(m.str());
        const auto name_pos = s.rfind(sig);
        if (name_pos != std::string::npos)
            s = trim(s.substr(0, name_pos));
        return s;
    }
    return "";
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
                                       const std::string& wire_name) {
    auto r = build_connect(analyzer, uri, source_path, source_port, dest_path, dest_port, wire_name);
    if (!r.error.empty())
        return error_json(r.error);
    return preview_json(wire_name, r.wire_type, r.lca_module, r.preview, r.warnings);
}

std::string connect_apply_edit_json(const Analyzer& analyzer, const std::string& uri,
                                    const std::string& source_path,
                                    const std::string& source_port,
                                    const std::string& dest_path,
                                    const std::string& dest_port,
                                    const std::string& wire_name) {
    auto r = build_connect(analyzer, uri, source_path, source_port, dest_path, dest_port, wire_name);
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
                   ",\"signal_type\":" + q(signal_type_in_text(a->first->text, c1.signal_name)) + "}";
            continue;
        }
        if (!first) out += ",";
        first = false;
        out += "{\"inst1_port\":" + q(c1.port_name) + ",\"inst2_port\":" + q(it->second.port_name) +
               ",\"signal\":" + q(c1.signal_name) + ",\"signal_type\":" + q(signal_type_in_text(a->first->text, c1.signal_name)) + "}";
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
                   ",\"signal\":" + q(sig) + ",\"signal_type\":" + q(signal_type_in_text(file.text, sig)) +
                   ",\"other_inst\":\"\",\"other_port\":\"\",\"other_dir\":\"\",\"other_type\":\"\"}";
        } else {
            for (auto it = range.first; it != range.second; ++it) {
                const auto& [oi, op, od, ot] = it->second;
                if (!first) out += ","; first = false;
                out += "{\"port_name\":" + q(p.name) + ",\"port_type\":" + q(decl_type_for_port(p)) + ",\"port_dir\":" + q(p.direction) +
                       ",\"signal\":" + q(sig) + ",\"signal_type\":" + q(signal_type_in_text(file.text, sig)) +
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
    if (auto e = add_wire_decl(*a->first, wire_name, declaration_type)) edits.push_back(*e);
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

    auto lines = split_lines(a->first->text);
    for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
        if (std::regex_search(lines[i], std::regex("^\\s*(?:wire|logic|reg)\\b[^;]*\\b" + signal_name + "\\b[^;]*;\\s*$"))) {
            edits.push_back(TextEdit{a->first->uri, i, 0, i + 1, 0, ""});
            break;
        }
    }
    return workspace_edit_json(edits);
}
