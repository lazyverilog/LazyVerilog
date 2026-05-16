#include "autofunc.hpp"
#include "../syntax_index.hpp"
#include <slang/syntax/AllSyntax.h>
#include <slang/syntax/SyntaxVisitor.h>
#include <slang/text/SourceManager.h>
#include <optional>
#include <algorithm>
#include <cctype>

using namespace slang;
using namespace slang::syntax;

// ── Helpers ───────────────────────────────────────────────────────────────────

static std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) {
            lines.push_back(text.substr(start));
            break;
        }
        lines.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    if (lines.empty())
        lines.push_back({});
    return lines;
}

// ── Find extent of call starting from identifier ─────────────────────────────

struct CallExtent {
    int start_line;
    int start_col;
    int end_line;
    int end_col;
};

static CallExtent find_call_extent(const std::vector<std::string>& lines, int line,
                                   int ident_start, int ident_end) {
    const std::string& first = lines[line];
    int open_pos = ident_end;
    while (open_pos < (int)first.size() && (first[open_pos] == ' ' || first[open_pos] == '\t'))
        ++open_pos;
    if (open_pos >= (int)first.size() || (first[open_pos] != '(' && first[open_pos] != ';'))
        return {line, ident_start, line, ident_end};
    if (first[open_pos] == ';')
        return {line, ident_start, line, ident_end};

    // Scan forward across lines until parens balance
    int depth = 0;
    for (int l = line; l < (int)lines.size(); ++l) {
        const std::string& ln = lines[l];
        int col_start = (l == line) ? open_pos : 0;
        for (int c = col_start; c < (int)ln.size(); ++c) {
            if (ln[c] == '(') ++depth;
            else if (ln[c] == ')') {
                --depth;
                if (depth <= 0) {
                    int end_col = c + 1;
                    // Consume trailing semicolon on same line
                    int pos = end_col;
                    while (pos < (int)ln.size() && (ln[pos] == ' ' || ln[pos] == '\t')) ++pos;
                    if (pos < (int)ln.size() && ln[pos] == ';') end_col = pos + 1;
                    return {line, ident_start, l, end_col};
                }
            }
        }
    }
    // Unbalanced — use end of last scanned line
    int last_line = (int)lines.size() - 1;
    int end_col = (int)lines[last_line].size();
    return {line, ident_start, last_line, end_col};
}

// ── Parse existing .port(wire) connections ────────────────────────────────────

static std::map<std::string, std::string> parse_existing_connections(
    const std::string& call_text,
    const std::vector<std::string>& ports)
{
    std::map<std::string, std::string> result;
    size_t i = 0;
    while (i < call_text.size()) {
        if (call_text[i] != '.') { ++i; continue; }
        size_t name_start = i + 1;
        if (name_start >= call_text.size() ||
            (!std::isalpha((unsigned char)call_text[name_start]) && call_text[name_start] != '_')) {
            ++i;
            continue;
        }
        size_t name_end = name_start + 1;
        while (name_end < call_text.size() &&
               (std::isalnum((unsigned char)call_text[name_end]) || call_text[name_end] == '_'))
            ++name_end;
        std::string port = call_text.substr(name_start, name_end - name_start);
        size_t start_paren = name_end;
        while (start_paren < call_text.size() &&
               (call_text[start_paren] == ' ' || call_text[start_paren] == '\t'))
            ++start_paren;
        if (start_paren >= call_text.size() || call_text[start_paren] != '(') {
            i = name_end;
            continue;
        }
        int depth = 0;
        size_t j = start_paren;
        while (j < call_text.size()) {
            if (call_text[j] == '(') ++depth;
            else if (call_text[j] == ')') {
                --depth;
                if (depth == 0) {
                    std::string wire = call_text.substr(start_paren + 1, j - start_paren - 1);
                    // trim
                    size_t s = wire.find_first_not_of(" \t\n\r");
                    size_t e = wire.find_last_not_of(" \t\n\r");
                    if (s != std::string::npos)
                        wire = wire.substr(s, e - s + 1);
                    else
                        wire.clear();
                    if (!wire.empty()) result[port] = wire;
                    i = j + 1;
                    break;
                }
            }
            ++j;
        }
        if (j >= call_text.size()) break;
    }

    size_t open_paren = call_text.find('(');
    if (open_paren == std::string::npos)
        return result;

    std::vector<std::string> positional_args;
    int depth = 0;
    size_t arg_start = open_paren + 1;
    for (size_t j = open_paren; j < call_text.size(); ++j) {
        char ch = call_text[j];
        if (ch == '(') {
            ++depth;
            continue;
        }
        if (ch == ')') {
            --depth;
            if (depth == 0) {
                positional_args.push_back(call_text.substr(arg_start, j - arg_start));
                break;
            }
            continue;
        }
        if (ch == ',' && depth == 1) {
            positional_args.push_back(call_text.substr(arg_start, j - arg_start));
            arg_start = j + 1;
        }
    }

    for (size_t arg_index = 0; arg_index < positional_args.size() && arg_index < ports.size();
         ++arg_index) {
        std::string wire = positional_args[arg_index];
        size_t s = wire.find_first_not_of(" \t\n\r");
        size_t e = wire.find_last_not_of(" \t\n\r");
        if (s == std::string::npos)
            continue;
        wire = wire.substr(s, e - s + 1);
        if (!wire.empty() && wire[0] != '.' && !result.contains(ports[arg_index]))
            result[ports[arg_index]] = wire;
    }

    return result;
}

// ── Find function/task ports via AST walk ─────────────────────────────────────

struct FuncPortFinder : public SyntaxVisitor<FuncPortFinder> {
    std::string target_name;
    std::optional<std::vector<std::string>> ports;

    explicit FuncPortFinder(const std::string& name) : target_name(name) {}

    void handle(const FunctionDeclarationSyntax& node) {
        if (ports) { visitDefault(node); return; }
        if (!node.prototype) { visitDefault(node); return; }
        std::string name = std::string(node.prototype->name->getFirstToken().valueText());
        if (name != target_name) { visitDefault(node); return; }

        std::vector<std::string> port_names;
        if (node.prototype->portList) {
            // Walk the port list for FunctionPortSyntax
            struct PortCollector : public SyntaxVisitor<PortCollector> {
                std::vector<std::string>& out;
                explicit PortCollector(std::vector<std::string>& o) : out(o) {}
                void handle(const FunctionPortSyntax& p) {
                    if (p.declarator) {
                        std::string pname = std::string(p.declarator->name.valueText());
                        if (!pname.empty()) out.push_back(pname);
                    }
                    visitDefault(p);
                }
            } pc(port_names);
            node.prototype->portList->visit(pc);
        }
        ports = std::move(port_names);
        visitDefault(node);
    }
};

static std::optional<std::vector<std::string>> find_func_or_task_ports(
    const Analyzer& analyzer, const std::string& symbol_name)
{
    std::optional<std::vector<std::string>> result;
    analyzer.for_each_state([&](const std::string&, const std::shared_ptr<const DocumentState>& state) {
        if (result || !state || !state->tree) return;
        FuncPortFinder finder(symbol_name);
        state->tree->root().visit(finder);
        if (finder.ports)
            result = std::move(finder.ports);
    });
    return result;
}

// ── Generate function call text ───────────────────────────────────────────────

static std::string generate_func_call(
    const std::string& name,
    const std::vector<std::string>& ports,
    const std::string& indent,
    const std::map<std::string, std::string>& wire_map,
    const AutoFuncOptions& options)
{
    if (ports.empty())
        return name + "();";

    int base_col = (int)indent.size();
    int indent_size = std::max(1, options.indent_size);
    int arg_col = ((base_col + indent_size) / indent_size) * indent_size;
    if (arg_col <= base_col) arg_col = base_col + indent_size;
    std::string arg_indent(arg_col, ' ');

    std::string out = name + "(\n";
    for (size_t i = 0; i < ports.size(); ++i) {
        const auto& p = ports[i];
        std::string wire = p;
        auto it = wire_map.find(p);
        if (it != wire_map.end()) wire = it->second;
        std::string comma = (i + 1 < ports.size()) ? "," : "";
        if (options.use_named_arguments)
            out += arg_indent + "." + p + "(" + wire + ")" + comma + "\n";
        else
            out += arg_indent + wire + comma + "\n";
    }
    out += indent + ");";
    return out;
}

// ── Main autofunc entry point ─────────────────────────────────────────────────

std::optional<lsWorkspaceEdit> autofunc(
    const Analyzer& analyzer, const std::string& uri,
    int line, int col, const AutoFuncOptions& options)
{
    auto state = analyzer.get_state(uri);
    if (!state) return std::nullopt;

    auto lines = split_lines(state->text);
    if (line < 0 || line >= (int)lines.size())
        return std::nullopt;

    const std::string& src_line = lines[line];

    // Find identifier at cursor via the parsed SyntaxTree.
    auto ident = analyzer.identifier_at(uri, line, col);
    if (!ident) return std::nullopt;

    // Find call extent (may span multiple lines)
    auto extent = find_call_extent(lines, line, ident->col, ident->end_col);

    // Look up function/task ports
    auto ports = find_func_or_task_ports(analyzer, ident->name);
    if (!ports || ports->empty())
        return std::nullopt;

    // Extract existing call text (possibly multi-line) for connection preservation
    std::string call_text;
    for (int l = extent.start_line; l <= extent.end_line; ++l) {
        int col_s = (l == extent.start_line) ? extent.start_col : 0;
        int col_e = (l == extent.end_line)   ? extent.end_col   : (int)lines[l].size();
        call_text += lines[l].substr(col_s, col_e - col_s);
        if (l < extent.end_line) call_text += '\n';
    }
    auto wire_map = parse_existing_connections(call_text, *ports);

    // Detect line indent
    std::string indent;
    for (char c : src_line) {
        if (c == ' ' || c == '\t') indent += c;
        else break;
    }

    // Generate replacement text
    std::string new_call = generate_func_call(ident->name, *ports, indent, wire_map, options);

    // Build workspace edit
    lsWorkspaceEdit we;
    lsTextEdit edit;
    edit.range.start = lsPosition(extent.start_line, extent.start_col);
    edit.range.end   = lsPosition(extent.end_line,   extent.end_col);
    edit.newText = new_call;
    we.changes = std::map<std::string, std::vector<lsTextEdit>>{};
    (*we.changes)[uri] = {edit};
    return we;
}
