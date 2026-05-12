#include "lint.hpp"
#include <slang/syntax/AllSyntax.h>
#include <slang/syntax/SyntaxVisitor.h>
#include <slang/text/SourceManager.h>
#include <slang/parsing/TokenKind.h>
#include <regex>
#include <optional>

using namespace slang;
using namespace slang::syntax;
using namespace slang::parsing;

static ParseDiagInfo make_diag(SourceManager& sm, SourceLocation loc,
                               int sev, std::string msg) {
    ParseDiagInfo d;
    d.severity = sev;
    d.message  = std::move(msg);
    if (loc.valid()) {
        size_t ln = sm.getLineNumber(loc);
        size_t co = sm.getColumnNumber(loc);
        d.line = ln > 0 ? (int)ln - 1 : 0;
        d.col  = co > 0 ? (int)co - 1 : 0;
    }
    return d;
}

// ── Naming helpers ────────────────────────────────────────────────────────────

enum class PortDir { None, Input, Output, InOut };

static PortDir get_port_direction(const PortHeaderSyntax& hdr) {
    Token dir;
    if (const auto* v = hdr.as_if<VariablePortHeaderSyntax>()) dir = v->direction;
    else if (const auto* n = hdr.as_if<NetPortHeaderSyntax>()) dir = n->direction;
    else return PortDir::None;
    if (!dir.valid()) return PortDir::None;
    switch (dir.kind) {
        case TokenKind::InputKeyword:  return PortDir::Input;
        case TokenKind::OutputKeyword: return PortDir::Output;
        case TokenKind::InOutKeyword:  return PortDir::InOut;
        default: return PortDir::None;
    }
}

static std::optional<std::regex> compile_re(const std::string& pat) {
    if (pat.empty()) return std::nullopt;
    try { return std::regex(pat); }
    catch (...) { return std::nullopt; }
}

/// Recursively check if a syntax subtree contains an if-without-else (latch risk).
static bool has_latch_risk(const SyntaxNode& node) {
    if (const auto* cond = node.as_if<ConditionalStatementSyntax>())
        if (!cond->elseClause)
            return true;
    for (uint32_t i = 0; i < node.getChildCount(); ++i) {
        const SyntaxNode* child = node.childNode(i);
        if (child && has_latch_risk(*child))
            return true;
    }
    return false;
}

struct LintVisitor : public SyntaxVisitor<LintVisitor> {
    const LintConfig&          cfg;
    SourceManager&             sm;
    std::vector<ParseDiagInfo> diags;
    bool in_always_ff_{false};

    // Compiled naming regexes
    std::optional<std::regex> module_re_;
    std::optional<std::regex> input_port_re_;
    std::optional<std::regex> output_port_re_;
    std::optional<std::regex> signal_re_;
    std::optional<std::regex> register_re_;

    LintVisitor(const LintConfig& c, SourceManager& s) : cfg(c), sm(s) {
        if (cfg.naming.enable) {
            module_re_      = compile_re(cfg.naming.module_pattern);
            input_port_re_  = compile_re(cfg.naming.input_port_pattern);
            output_port_re_ = compile_re(cfg.naming.output_port_pattern);
            signal_re_      = compile_re(cfg.naming.signal_pattern);
            register_re_    = compile_re(cfg.naming.register_pattern);
        }
    }

    int naming_sev() const {
        if (cfg.naming.severity == "error") return 1;
        if (cfg.naming.severity == "hint")  return 3;
        return 2;
    }

    void chk_name(const std::string& name, SourceLocation loc,
                  const std::optional<std::regex>& re,
                  const std::string& pat, const char* cat) {
        if (!re || name.empty()) return;
        if (!std::regex_match(name, *re))
            diags.push_back(make_diag(sm, loc, naming_sev(),
                std::string("[naming] ") + cat + " '" + name + "' does not match pattern '" + pat + "'"));
    }

    void chk_port(const std::string& name, SourceLocation loc, PortDir dir) {
        if (!cfg.naming.enable) return;
        if (dir == PortDir::Input)
            chk_name(name, loc, input_port_re_, cfg.naming.input_port_pattern, "input port");
        else if (dir == PortDir::Output)
            chk_name(name, loc, output_port_re_, cfg.naming.output_port_pattern, "output port");
    }

    // ── case_missing_default ──────────────────────────────────────────────
    void handle(const CaseStatementSyntax& node) {
        if (cfg.case_missing_default && !node.uniqueOrPriority.valid()) {
            bool has_default = false;
            for (uint32_t i = 0; i < node.items.size() && !has_default; ++i)
                if (node.items[i]->as_if<DefaultCaseItemSyntax>())
                    has_default = true;
            if (!has_default)
                diags.push_back(make_diag(sm, node.caseKeyword.location(), 2,
                    "[statement] case statement missing default item"));
        }
        visitDefault(node);
    }

    // ── functions_automatic / explicit_function_lifetime / explicit_task_lifetime
    void handle(const FunctionDeclarationSyntax& node) {
        bool is_task   = (node.kind == SyntaxKind::TaskDeclaration);
        auto& proto    = *node.prototype;
        bool  has_life = proto.lifetime.valid() && !proto.lifetime.rawText().empty();

        if (!is_task) {
            if (cfg.functions_automatic) {
                bool is_auto = has_life && proto.lifetime.rawText() == "automatic";
                if (!is_auto)
                    diags.push_back(make_diag(sm, proto.keyword.location(), 2,
                        "[function] function declaration should use 'automatic' lifetime"));
            } else if (cfg.explicit_function_lifetime && !has_life) {
                diags.push_back(make_diag(sm, proto.keyword.location(), 2,
                    "[function] function declaration missing explicit lifetime (automatic/static)"));
            }
        } else if (cfg.explicit_task_lifetime && !has_life) {
            diags.push_back(make_diag(sm, proto.keyword.location(), 2,
                "[function] task declaration missing explicit lifetime (automatic/static)"));
        }
        visitDefault(node);
    }

    // ── module_instantiation_style (bool: true → require named connections) ─
    void handle(const HierarchyInstantiationSyntax& node) {
        if (cfg.module_instantiation_style) {
            for (uint32_t i = 0; i < node.instances.size(); ++i) {
                const auto* inst = node.instances[i];
                if (!inst) continue;
                bool positional = false;
                for (uint32_t j = 0; j < inst->connections.size() && !positional; ++j) {
                    if (const auto* conn = inst->connections[j])
                        if (conn->as_if<OrderedPortConnectionSyntax>())
                            positional = true;
                }
                if (positional)
                    diags.push_back(make_diag(sm, node.type.location(), 2,
                        "[module] instance uses positional port connections; named connections preferred"));
            }
        }
        visitDefault(node);
    }

    // ── naming: module name ───────────────────────────────────────────────────
    void handle(const ModuleDeclarationSyntax& node) {
        if (cfg.naming.enable && module_re_ && node.kind == SyntaxKind::ModuleDeclaration) {
            auto name = std::string(node.header->name.valueText());
            chk_name(name, node.header->name.location(), module_re_,
                     cfg.naming.module_pattern, "module");
        }
        visitDefault(node);
    }

    // ── naming: ANSI port names ───────────────────────────────────────────────
    void handle(const ImplicitAnsiPortSyntax& node) {
        if (cfg.naming.enable && (input_port_re_ || output_port_re_)) {
            PortDir dir = get_port_direction(*node.header);
            auto name = std::string(node.declarator->name.valueText());
            chk_port(name, node.declarator->name.location(), dir);
        }
        visitDefault(node);
    }

    // ── naming: non-ANSI port declarations ───────────────────────────────────
    void handle(const PortDeclarationSyntax& node) {
        if (cfg.naming.enable && (input_port_re_ || output_port_re_)) {
            PortDir dir = get_port_direction(*node.header);
            for (uint32_t i = 0; i < node.declarators.size(); ++i) {
                const auto* decl = node.declarators[i];
                if (!decl) continue;
                auto name = std::string(decl->name.valueText());
                chk_port(name, decl->name.location(), dir);
            }
        }
        visitDefault(node);
    }

    // ── naming: signal names (logic/wire/var declarations) ───────────────────
    void handle(const DataDeclarationSyntax& node) {
        if (cfg.naming.enable && signal_re_) {
            for (uint32_t i = 0; i < node.declarators.size(); ++i) {
                const auto* decl = node.declarators[i];
                if (!decl) continue;
                auto name = std::string(decl->name.valueText());
                chk_name(name, decl->name.location(), signal_re_,
                         cfg.naming.signal_pattern, "signal");
            }
        }
        visitDefault(node);
    }

    void handle(const NetDeclarationSyntax& node) {
        if (cfg.naming.enable && signal_re_) {
            for (uint32_t i = 0; i < node.declarators.size(); ++i) {
                const auto* decl = node.declarators[i];
                if (!decl) continue;
                auto name = std::string(decl->name.valueText());
                chk_name(name, decl->name.location(), signal_re_,
                         cfg.naming.signal_pattern, "signal");
            }
        }
        visitDefault(node);
    }

    // ── latch_inference_detection + register naming (always_ff) ──────────────
    void handle(const ProceduralBlockSyntax& node) {
        if (cfg.latch_inference_detection && node.kind == SyntaxKind::AlwaysCombBlock) {
            if (has_latch_risk(*node.statement))
                diags.push_back(make_diag(sm, node.keyword.location(), 2,
                    "[statement] always_comb block may infer a latch (incomplete if)"));
        }
        bool was_ff = in_always_ff_;
        if (node.kind == SyntaxKind::AlwaysFFBlock)
            in_always_ff_ = true;
        visitDefault(node);
        in_always_ff_ = was_ff;
    }

    // ── naming: register names (nonblocking assignment LHS in always_ff) ─────
    void handle(const BinaryExpressionSyntax& node) {
        if (in_always_ff_ && cfg.naming.enable && register_re_ &&
            node.kind == SyntaxKind::NonblockingAssignmentExpression) {
            Token tok = node.left->getFirstToken();
            if (tok.valid() && tok.kind == TokenKind::Identifier) {
                auto name = std::string(tok.valueText());
                chk_name(name, tok.location(), register_re_,
                         cfg.naming.register_pattern, "register");
            }
        }
        visitDefault(node);
    }
};

std::vector<ParseDiagInfo> run_lint(const DocumentState& state, const LintConfig& config) {
    if (!state.tree)
        return {};
    // Fast path: skip if no rules are enabled
    bool naming_active = config.naming.enable && (
        !config.naming.module_pattern.empty() ||
        !config.naming.input_port_pattern.empty() ||
        !config.naming.output_port_pattern.empty() ||
        !config.naming.signal_pattern.empty() ||
        !config.naming.register_pattern.empty());
    if (!config.case_missing_default && !config.functions_automatic &&
        !config.explicit_function_lifetime && !config.explicit_task_lifetime &&
        !config.module_instantiation_style && !config.latch_inference_detection &&
        !config.explicit_begin && !config.register_naming && !naming_active)
        return {};

    auto& sm = state.tree->sourceManager();
    LintVisitor v(config, sm);
    state.tree->root().visit(v);
    return std::move(v.diags);
}
