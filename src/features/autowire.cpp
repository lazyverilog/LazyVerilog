#include "autowire.hpp"
#include <slang/syntax/AllSyntax.h>
#include <slang/syntax/SyntaxVisitor.h>
#include <slang/text/SourceManager.h>
#include <regex>
#include <algorithm>
#include <set>

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

static bool is_simple_id(const std::string& s) {
    if (s.empty()) return false;
    if (!std::isalpha((unsigned char)s[0]) && s[0] != '_') return false;
    for (char c : s)
        if (!std::isalnum((unsigned char)c) && c != '_') return false;
    return true;
}

// ── AST visitor: collect declared signal names ────────────────────────────────

struct DeclCollector : public SyntaxVisitor<DeclCollector> {
    std::set<std::string> declared;

    void handle(const DataDeclarationSyntax& node) {
        for (uint32_t i = 0; i < node.declarators.size(); ++i) {
            const auto* d = node.declarators[i];
            if (d) declared.insert(std::string(d->name.valueText()));
        }
        visitDefault(node);
    }
    void handle(const NetDeclarationSyntax& node) {
        for (uint32_t i = 0; i < node.declarators.size(); ++i) {
            const auto* d = node.declarators[i];
            if (d) declared.insert(std::string(d->name.valueText()));
        }
        visitDefault(node);
    }
    void handle(const PortDeclarationSyntax& node) {
        for (uint32_t i = 0; i < node.declarators.size(); ++i) {
            const auto* d = node.declarators[i];
            if (d) declared.insert(std::string(d->name.valueText()));
        }
        visitDefault(node);
    }
    void handle(const ImplicitAnsiPortSyntax& node) {
        if (node.declarator)
            declared.insert(std::string(node.declarator->name.valueText()));
        visitDefault(node);
    }
};

// ── AST visitor: collect assign LHS signals ───────────────────────────────────

struct AssignLhsCollector : public SyntaxVisitor<AssignLhsCollector> {
    std::vector<std::string> signals;

    void handle(const ContinuousAssignSyntax& node) {
        for (uint32_t i = 0; i < node.assignments.size(); ++i) {
            const auto* expr = node.assignments[i];
            if (!expr) continue;
            const auto* assign = expr->as_if<BinaryExpressionSyntax>();
            if (!assign || assign->kind != SyntaxKind::AssignmentExpression) continue;
            const auto* id = assign->left->as_if<IdentifierNameSyntax>();
            if (!id) continue;
            std::string name = std::string(id->identifier.valueText());
            if (is_simple_id(name))
                signals.push_back(name);
        }
        visitDefault(node);
    }
};

// ── AST visitor: collect always_comb LHS signals ─────────────────────────────

struct CombLhsCollector : public SyntaxVisitor<CombLhsCollector> {
    std::vector<std::string> signals;
    bool in_comb{false};

    void handle(const ProceduralBlockSyntax& node) {
        if (node.kind == SyntaxKind::AlwaysCombBlock) {
            bool was = in_comb;
            in_comb = true;
            visitDefault(node);
            in_comb = was;
        } else {
            visitDefault(node);
        }
    }
    void handle(const BinaryExpressionSyntax& node) {
        if (in_comb && node.kind == SyntaxKind::AssignmentExpression) {
            const auto* id = node.left->as_if<IdentifierNameSyntax>();
            if (id) {
                std::string name = std::string(id->identifier.valueText());
                if (is_simple_id(name))
                    signals.push_back(name);
            }
        }
        visitDefault(node);
    }
};

// ── Scan instantiation source text for .port(signal) connections ───────────────

static const std::regex PORT_CONN_RE(R"(\.(\w+)\s*\(([^)]*)\))");

struct InstSignal {
    std::string signal;
    std::string module_name;
    std::string type_kw;
    std::string dimension;
    int order;
};

static std::vector<InstSignal> collect_inst_signals(
    const std::vector<std::string>& lines,
    const SyntaxIndex& syntax_index)
{
    std::vector<InstSignal> results;
    std::set<std::string> seen;
    int order = 0;

    for (const auto& inst : syntax_index.instances) {
        // Find module definition for port info
        const ModuleEntry* mod_entry = nullptr;
        for (const auto& m : syntax_index.modules) {
            if (m.name == inst.module_name) {
                mod_entry = &m;
                break;
            }
        }

        // Scan source lines for .port(signal) connections
        for (int i = inst.start_line; i <= inst.end_line && i < (int)lines.size(); ++i) {
            const std::string& raw = lines[i];
            auto begin = std::sregex_iterator(raw.begin(), raw.end(), PORT_CONN_RE);
            auto end_it = std::sregex_iterator();
            for (auto it = begin; it != end_it; ++it) {
                std::string port_name = (*it)[1].str();
                std::string signal = (*it)[2].str();
                // trim signal
                size_t s = signal.find_first_not_of(" \t");
                size_t e = signal.find_last_not_of(" \t");
                if (s != std::string::npos)
                    signal = signal.substr(s, e - s + 1);
                else
                    signal.clear();

                if (!is_simple_id(signal)) continue;
                if (seen.count(signal)) continue;

                // Look up port direction
                std::string direction;
                std::string type_kw = "logic";
                std::string dimension;
                if (mod_entry) {
                    for (const auto& p : mod_entry->ports) {
                        if (p.name == port_name) {
                            direction = p.direction;
                            // Extract dimension from type string
                            std::regex dim_re(R"(\[.*?\])");
                            std::smatch dm;
                            if (std::regex_search(p.type, dm, dim_re))
                                dimension = dm[0].str();
                            break;
                        }
                    }
                }

                // Only include output/inout ports
                if (direction == "input") continue;
                if (!direction.empty() && direction != "output" && direction != "inout" &&
                    direction != "out" && direction != "Out") continue;

                seen.insert(signal);
                results.push_back({signal, inst.module_name, type_kw, dimension, order++});
            }
        }
    }
    return results;
}

// ── Find module body range ────────────────────────────────────────────────────

static std::pair<int, int> find_module_body_range(const std::vector<std::string>& lines) {
    static const std::regex mod_re(R"(\s*module\b)", std::regex::icase);
    static const std::regex endmod_re(R"(\s*endmodule\b)", std::regex::icase);

    int mod_line = -1, endmod_line = -1;
    for (int i = 0; i < (int)lines.size(); ++i) {
        if (mod_line == -1 && std::regex_search(lines[i], mod_re))
            mod_line = i;
        if (std::regex_search(lines[i], endmod_re)) {
            endmod_line = i;
            break;
        }
    }
    if (mod_line < 0 || endmod_line < 0)
        return {0, (int)lines.size() - 1};

    // Find end of module header (after semicolon)
    int header_end = mod_line;
    for (int i = mod_line; i < endmod_line; ++i) {
        if (lines[i].find(';') != std::string::npos) {
            header_end = i;
            break;
        }
    }
    return {header_end + 1, endmod_line};
}

static int find_insertion_line(const std::vector<std::string>& lines) {
    auto [body_start, endmod_line] = find_module_body_range(lines);

    // Priority 1: After last wire/logic/reg declaration
    static const std::regex decl_re(R"(^\s*(?:wire|logic|reg|tri)\b)");
    int last_decl = -1;
    for (int i = body_start; i < endmod_line; ++i) {
        if (std::regex_search(lines[i], decl_re))
            last_decl = i;
    }
    if (last_decl >= 0)
        return last_decl + 1;

    // Priority 2: Before first instantiation
    static const std::set<std::string> KEYWORDS = {
        "module", "endmodule", "input", "output", "inout", "wire", "logic",
        "reg", "assign", "always", "always_comb", "always_ff", "always_latch",
        "initial", "generate", "endgenerate", "if", "else", "begin", "end",
        "for", "while", "case", "endcase", "function", "endfunction",
        "task", "endtask", "parameter", "localparam", "typedef", "enum",
        "struct", "union", "interface", "endinterface", "package", "endpackage",
    };
    static const std::regex inst_re(R"(^\s*(\w+)\s+(?:#\s*\(|(\w+)\s*(?:\(|$)))");
    for (int i = body_start; i < endmod_line; ++i) {
        std::smatch m;
        if (std::regex_search(lines[i], m, inst_re)) {
            if (!KEYWORDS.count(m[1].str()))
                return i;
        }
    }

    // Priority 3: Before first begin
    static const std::regex begin_re(R"(\s*begin\b)");
    for (int i = body_start; i < endmod_line; ++i) {
        if (std::regex_search(lines[i], begin_re))
            return i;
    }

    return body_start;
}

// ── Format declarations ───────────────────────────────────────────────────────

struct SignalDecl {
    std::string name;
    std::string type_kw;
    std::string dimension;
    std::string module_name;
    int order;
};

static std::string format_one_decl(const SignalDecl& s, size_t max_dim_len) {
    if (!s.dimension.empty()) {
        std::string dim_part = s.dimension;
        while (dim_part.size() < max_dim_len) dim_part += ' ';
        return s.type_kw + " " + dim_part + " " + s.name + ";";
    } else if (max_dim_len > 0) {
        std::string pad(max_dim_len, ' ');
        return s.type_kw + " " + pad + " " + s.name + ";";
    } else {
        return s.type_kw + " " + s.name + ";";
    }
}

static std::string format_declarations(const std::vector<SignalDecl>& signals) {
    if (signals.empty()) return {};
    size_t max_dim_len = 0;
    for (const auto& s : signals)
        max_dim_len = std::max(max_dim_len, s.dimension.size());

    std::string out;
    for (size_t i = 0; i < signals.size(); ++i) {
        if (i > 0) out += "\n";
        out += format_one_decl(signals[i], max_dim_len);
    }
    return out;
}

// ── Main entry points ─────────────────────────────────────────────────────────

static std::vector<SignalDecl> compute_new_signals(
    const DocumentState& state, const SyntaxIndex& syntax_index)
{
    if (!state.tree)
        return {};

    // Collect declared signals
    DeclCollector decl_coll;
    state.tree->root().visit(decl_coll);
    // Also collect from params/localparams via regex (simple)
    static const std::regex param_re(R"(^\s*(?:parameter|localparam)\b.*?\b(\w+)\s*=)",
                                     std::regex::multiline);
    auto lines = split_lines(state.text);
    for (const auto& ln : lines) {
        std::smatch pm;
        std::string::const_iterator sb = ln.cbegin();
        while (std::regex_search(sb, ln.cend(), pm, param_re)) {
            decl_coll.declared.insert(pm[1].str());
            sb = pm.suffix().first;
        }
    }

    // Collect assign LHS
    AssignLhsCollector assign_coll;
    state.tree->root().visit(assign_coll);

    // Collect always_comb LHS
    CombLhsCollector comb_coll;
    state.tree->root().visit(comb_coll);

    // Collect instantiation signals
    auto inst_sigs = collect_inst_signals(lines, syntax_index);

    // Build result: filter out already declared
    std::set<std::string> seen;
    std::vector<SignalDecl> result;
    int order = 0;

    for (const auto& is : inst_sigs) {
        if (seen.count(is.signal) || decl_coll.declared.count(is.signal)) {
            seen.insert(is.signal);
            continue;
        }
        seen.insert(is.signal);
        result.push_back({is.signal, is.type_kw, is.dimension, is.module_name, order++});
    }
    for (const auto& name : assign_coll.signals) {
        if (seen.count(name) || decl_coll.declared.count(name)) {
            seen.insert(name);
            continue;
        }
        seen.insert(name);
        result.push_back({name, "logic", "", "__assign__", order++});
    }
    for (const auto& name : comb_coll.signals) {
        if (seen.count(name) || decl_coll.declared.count(name)) {
            seen.insert(name);
            continue;
        }
        seen.insert(name);
        result.push_back({name, "logic", "", "__always_comb__", order++});
    }

    // Sort by order
    std::sort(result.begin(), result.end(), [](const SignalDecl& a, const SignalDecl& b) {
        return a.order < b.order;
    });
    return result;
}

std::string autowire_apply(
    const DocumentState& state, const SyntaxIndex& syntax_index,
    const AutowireOptions& /*options*/)
{
    auto new_sigs = compute_new_signals(state, syntax_index);
    if (new_sigs.empty())
        return state.text;

    auto lines = split_lines(state.text);
    std::string decl_text = format_declarations(new_sigs);
    int insert_line = find_insertion_line(lines);

    std::vector<std::string> out_lines;
    out_lines.insert(out_lines.end(), lines.begin(), lines.begin() + insert_line);
    for (const auto& dl : split_lines(decl_text))
        out_lines.push_back(dl);
    out_lines.push_back(""); // blank line after
    out_lines.insert(out_lines.end(), lines.begin() + insert_line, lines.end());

    std::string result;
    for (size_t i = 0; i < out_lines.size(); ++i) {
        if (i > 0) result += "\n";
        result += out_lines[i];
    }
    return result;
}

std::vector<std::string> autowire_preview(
    const DocumentState& state, const SyntaxIndex& syntax_index,
    const AutowireOptions& /*options*/)
{
    auto new_sigs = compute_new_signals(state, syntax_index);
    std::vector<std::string> out;
    if (!new_sigs.empty()) {
        out.push_back("Will add:");
        for (const auto& s : new_sigs) {
            std::string line = "  " + s.type_kw;
            if (!s.dimension.empty()) line += " " + s.dimension;
            line += " " + s.name + ";";
            out.push_back(line);
        }
    }
    return out;
}
