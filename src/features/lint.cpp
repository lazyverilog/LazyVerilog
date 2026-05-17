#include "lint.hpp"
#include <slang/syntax/AllSyntax.h>
#include <slang/syntax/SyntaxTree.h>
#include <slang/syntax/SyntaxVisitor.h>
#include <slang/text/SourceManager.h>
#include <slang/parsing/TokenKind.h>
#include <filesystem>
#include <regex>
#include <optional>
#include <unordered_set>

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

static std::vector<ParseDiagInfo> lint_trailing_whitespace(const std::string& text) {
    std::vector<ParseDiagInfo> diags;
    size_t line_start = 0;
    int line = 0;

    while (line_start < text.size()) {
        size_t line_end = text.find('\n', line_start);
        bool has_newline = line_end != std::string::npos;
        if (!has_newline)
            line_end = text.size();

        size_t content_end = line_end;
        if (content_end > line_start && text[content_end - 1] == '\r')
            --content_end;

        size_t trailing_start = content_end;
        while (trailing_start > line_start &&
               (text[trailing_start - 1] == ' ' || text[trailing_start - 1] == '\t')) {
            --trailing_start;
        }

        if (trailing_start < content_end) {
            ParseDiagInfo d;
            d.line = line;
            d.col = static_cast<int>(trailing_start - line_start);
            d.severity = 2;
            d.message = "[style] trailing whitespace";
            diags.push_back(std::move(d));
        }

        if (!has_newline)
            break;
        line_start = line_end + 1;
        ++line;
    }

    return diags;
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

static int severity_from(const std::string& severity) {
    if (severity == "error") return 1;
    if (severity == "hint") return 3;
    return 2;
}

static bool is_block_statement(const SyntaxNode* node) {
    return node && node->as_if<BlockStatementSyntax>() != nullptr;
}

static bool is_blocking_assignment(SyntaxKind kind) {
    static const std::unordered_set<SyntaxKind> kinds = {
        SyntaxKind::AssignmentExpression,
        SyntaxKind::AddAssignmentExpression,
        SyntaxKind::SubtractAssignmentExpression,
        SyntaxKind::MultiplyAssignmentExpression,
        SyntaxKind::DivideAssignmentExpression,
        SyntaxKind::ModAssignmentExpression,
        SyntaxKind::AndAssignmentExpression,
        SyntaxKind::OrAssignmentExpression,
        SyntaxKind::XorAssignmentExpression,
        SyntaxKind::LogicalLeftShiftAssignmentExpression,
        SyntaxKind::LogicalRightShiftAssignmentExpression,
        SyntaxKind::ArithmeticLeftShiftAssignmentExpression,
        SyntaxKind::ArithmeticRightShiftAssignmentExpression,
    };
    return kinds.count(kind) != 0;
}

static std::string file_stem_from_uri(const std::string& uri) {
    std::string path = uri;
    constexpr std::string_view prefix = "file://";
    if (path.rfind(prefix, 0) == 0)
        path.erase(0, prefix.size());
    return std::filesystem::path(path).stem().string();
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
    const SyntaxIndex&         index;
    SourceManager&             sm;
    std::vector<ParseDiagInfo> diags;
    bool in_always_ff_{false};
    bool in_always_comb_{false};
    int module_count_{0};
    std::string file_stem_;

    // Compiled naming regexes
    std::optional<std::regex> module_re_;
    std::optional<std::regex> input_port_re_;
    std::optional<std::regex> output_port_re_;
    std::optional<std::regex> signal_re_;
    std::optional<std::regex> register_re_;
    std::optional<std::regex> interface_re_;
    std::optional<std::regex> struct_re_;
    std::optional<std::regex> union_re_;
    std::optional<std::regex> enum_re_;
    std::optional<std::regex> parameter_re_;
    std::optional<std::regex> localparam_re_;

    LintVisitor(const LintConfig& c, const SyntaxIndex& idx, SourceManager& s, std::string file_stem)
        : cfg(c), index(idx), sm(s), file_stem_(std::move(file_stem)) {
        if (cfg.naming.enable) {
            module_re_      = compile_re(cfg.naming.module_pattern);
            input_port_re_  = compile_re(cfg.naming.input_port_pattern);
            output_port_re_ = compile_re(cfg.naming.output_port_pattern);
            signal_re_      = compile_re(cfg.naming.signal_pattern);
            register_re_    = compile_re(cfg.naming.register_pattern);
            interface_re_   = compile_re(cfg.naming.interface_pattern);
            struct_re_      = compile_re(cfg.naming.struct_pattern);
            union_re_       = compile_re(cfg.naming.union_pattern);
            enum_re_        = compile_re(cfg.naming.enum_pattern);
            parameter_re_   = compile_re(cfg.naming.parameter_pattern);
            localparam_re_  = compile_re(cfg.naming.localparam_pattern);
        }
    }

    int naming_sev() const { return severity_from(cfg.naming.severity); }
    int module_sev() const { return severity_from(cfg.module.severity); }
    int statement_sev() const { return severity_from(cfg.statement.severity); }

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
        if (cfg.statement.case_missing_default && !node.uniqueOrPriority.valid()) {
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
            if (cfg.function.functions_automatic) {
                bool is_auto = has_life && proto.lifetime.rawText() == "automatic";
                if (!is_auto)
                    diags.push_back(make_diag(sm, proto.keyword.location(), 2,
                        "[function] function declaration should use 'automatic' lifetime"));
            } else if (cfg.function.explicit_function_lifetime && !has_life) {
                diags.push_back(make_diag(sm, proto.keyword.location(), 2,
                    "[function] function declaration missing explicit lifetime (automatic/static)"));
            }
        } else if (cfg.function.explicit_task_lifetime && !has_life) {
            diags.push_back(make_diag(sm, proto.keyword.location(), 2,
                "[function] task declaration missing explicit lifetime (automatic/static)"));
        }
        visitDefault(node);
    }

    // ── module_instantiation_style (bool: true → require named connections) ─
    void handle(const HierarchyInstantiationSyntax& node) {
        if (!cfg.module.module_instantiation_style.empty()) {
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
        if (cfg.module.stale_autoinst_diagnostic) {
            auto module_it = index.module_by_name.find(std::string(node.type.valueText()));
            if (module_it != index.module_by_name.end() && module_it->second < index.modules.size()) {
                const auto& module = index.modules[module_it->second];
                for (uint32_t i = 0; i < node.instances.size(); ++i) {
                    const auto* inst = node.instances[i];
                    if (!inst) continue;
                    std::unordered_set<std::string> seen;
                    for (uint32_t j = 0; j < inst->connections.size(); ++j) {
                        const auto* named = inst->connections[j]
                                                ? inst->connections[j]->as_if<NamedPortConnectionSyntax>()
                                                : nullptr;
                        if (!named) continue;
                        std::string port = std::string(named->name.valueText());
                        if (!seen.insert(port).second)
                            diags.push_back(make_diag(sm, named->name.location(), module_sev(),
                                "[module] duplicate autoinst connection for port '" + port + "'"));
                        else if (!module.port_by_name.count(port))
                            diags.push_back(make_diag(sm, named->name.location(), module_sev(),
                                "[module] stale autoinst connection for unknown port '" + port + "'"));
                    }
                    for (const auto& port : module.ports) {
                        if (!seen.count(port.name) && inst->decl)
                            diags.push_back(make_diag(sm, inst->decl->name.location(), module_sev(),
                                "[module] autoinst connection missing port '" + port.name + "'"));
                    }
                }
            }
        }
        visitDefault(node);
    }

    // ── naming: module name ───────────────────────────────────────────────────
    void handle(const ModuleDeclarationSyntax& node) {
        auto name = std::string(node.header->name.valueText());
        if (node.kind == SyntaxKind::ModuleDeclaration) {
            ++module_count_;
            if (cfg.module.one_module_per_file && module_count_ > 1)
                diags.push_back(make_diag(sm, node.header->name.location(), module_sev(),
                    "[module] more than one module declared in this file"));
            if (cfg.naming.enable) {
                chk_name(name, node.header->name.location(), module_re_,
                         cfg.naming.module_pattern, "module");
                if (cfg.naming.check_module_filename && !file_stem_.empty() && name != file_stem_)
                    diags.push_back(make_diag(sm, node.header->name.location(), naming_sev(),
                        "[naming] module '" + name + "' does not match filename '" + file_stem_ + "'"));
            }
        } else if (node.kind == SyntaxKind::InterfaceDeclaration) {
            if (cfg.naming.enable)
                chk_name(name, node.header->name.location(), interface_re_,
                         cfg.naming.interface_pattern, "interface");
        } else if (node.kind == SyntaxKind::PackageDeclaration) {
            if (cfg.naming.enable && cfg.naming.check_package_filename && !file_stem_.empty() &&
                name != file_stem_)
                diags.push_back(make_diag(sm, node.header->name.location(), naming_sev(),
                    "[naming] package '" + name + "' does not match filename '" + file_stem_ + "'"));
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

    void handle(const TypedefDeclarationSyntax& node) {
        if (cfg.naming.enable) {
            if (node.type->kind == SyntaxKind::StructType)
                chk_name(std::string(node.name.valueText()), node.name.location(), struct_re_,
                         cfg.naming.struct_pattern, "struct");
            else if (node.type->kind == SyntaxKind::UnionType)
                chk_name(std::string(node.name.valueText()), node.name.location(), union_re_,
                         cfg.naming.union_pattern, "union");
            else if (node.type->kind == SyntaxKind::EnumType)
                chk_name(std::string(node.name.valueText()), node.name.location(), enum_re_,
                         cfg.naming.enum_pattern, "enum");
        }
        visitDefault(node);
    }

    void handle(const ParameterDeclarationSyntax& node) {
        if (cfg.naming.enable) {
            bool is_localparam = node.keyword.kind == TokenKind::LocalParamKeyword;
            const auto& re = is_localparam ? localparam_re_ : parameter_re_;
            const auto& pat = is_localparam ? cfg.naming.localparam_pattern : cfg.naming.parameter_pattern;
            const char* cat = is_localparam ? "localparam" : "parameter";
            for (uint32_t i = 0; i < node.declarators.size(); ++i) {
                const auto* decl = node.declarators[i];
                if (!decl) continue;
                chk_name(std::string(decl->name.valueText()), decl->name.location(), re, pat, cat);
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
        if (cfg.statement.no_raw_always && node.kind == SyntaxKind::AlwaysBlock)
            diags.push_back(make_diag(sm, node.keyword.location(), statement_sev(),
                "[statement] raw always block should use always_comb, always_ff, or always_latch"));
        if (cfg.statement.latch_inference_detection && node.kind == SyntaxKind::AlwaysCombBlock) {
            if (has_latch_risk(*node.statement))
                diags.push_back(make_diag(sm, node.keyword.location(), 2,
                    "[statement] always_comb block may infer a latch (incomplete if)"));
        }
        bool was_ff = in_always_ff_;
        bool was_comb = in_always_comb_;
        if (node.kind == SyntaxKind::AlwaysFFBlock)
            in_always_ff_ = true;
        if (node.kind == SyntaxKind::AlwaysCombBlock)
            in_always_comb_ = true;
        visitDefault(node);
        in_always_ff_ = was_ff;
        in_always_comb_ = was_comb;
    }

    // ── naming: register names (nonblocking assignment LHS in always_ff) ─────
    void handle(const BinaryExpressionSyntax& node) {
        if (cfg.statement.blocking_nonblocking_assignments) {
            if (in_always_ff_ && is_blocking_assignment(node.kind))
                diags.push_back(make_diag(sm, node.getFirstToken().location(), statement_sev(),
                    "[statement] always_ff should use nonblocking assignments"));
            else if (in_always_comb_ && node.kind == SyntaxKind::NonblockingAssignmentExpression)
                diags.push_back(make_diag(sm, node.getFirstToken().location(), statement_sev(),
                    "[statement] always_comb should use blocking assignments"));
        }
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

    void handle(const ConditionalStatementSyntax& node) {
        if (cfg.statement.explicit_begin) {
            if (!is_block_statement(node.statement))
                diags.push_back(make_diag(sm, node.ifKeyword.location(), statement_sev(),
                    "[statement] if statement body should use begin/end"));
            if (node.elseClause && !node.elseClause->clause->as_if<ConditionalStatementSyntax>() &&
                !is_block_statement(node.elseClause->clause))
                diags.push_back(make_diag(sm, node.elseClause->elseKeyword.location(), statement_sev(),
                    "[statement] else statement body should use begin/end"));
        }
        visitDefault(node);
    }

    void handle(const LoopStatementSyntax& node) {
        if (cfg.statement.explicit_begin && !is_block_statement(node.statement))
            diags.push_back(make_diag(sm, node.repeatOrWhile.location(), statement_sev(),
                "[statement] loop body should use begin/end"));
        visitDefault(node);
    }

    void handle(const ForLoopStatementSyntax& node) {
        if (cfg.statement.explicit_begin && !is_block_statement(node.statement))
            diags.push_back(make_diag(sm, node.forKeyword.location(), statement_sev(),
                "[statement] for loop body should use begin/end"));
        visitDefault(node);
    }

    void handle(const ForeachLoopStatementSyntax& node) {
        if (cfg.statement.explicit_begin && !is_block_statement(node.statement))
            diags.push_back(make_diag(sm, node.keyword.location(), statement_sev(),
                "[statement] foreach loop body should use begin/end"));
        visitDefault(node);
    }

    void handle(const ForeverStatementSyntax& node) {
        if (cfg.statement.explicit_begin && !is_block_statement(node.statement))
            diags.push_back(make_diag(sm, node.foreverKeyword.location(), statement_sev(),
                "[statement] forever body should use begin/end"));
        visitDefault(node);
    }

    void handle(const DoWhileStatementSyntax& node) {
        if (cfg.statement.explicit_begin && !is_block_statement(node.statement))
            diags.push_back(make_diag(sm, node.doKeyword.location(), statement_sev(),
                "[statement] do body should use begin/end"));
        visitDefault(node);
    }
};

std::vector<ParseDiagInfo> run_lint(const DocumentState& state, const LintConfig& config,
                                    const SyntaxIndex* merged_index) {
    std::vector<ParseDiagInfo> diags;
    if (!config.enable)
        return diags;
    if (config.style.trailing_whitespace)
        diags = lint_trailing_whitespace(state.text);

    if (!state.tree)
        return diags;
    // Fast path: skip if no rules are enabled
    bool naming_active = config.naming.enable && (
        !config.naming.module_pattern.empty() ||
        !config.naming.input_port_pattern.empty() ||
        !config.naming.output_port_pattern.empty() ||
        !config.naming.signal_pattern.empty() ||
        !config.naming.register_pattern.empty() ||
        !config.naming.interface_pattern.empty() ||
        !config.naming.struct_pattern.empty() ||
        !config.naming.union_pattern.empty() ||
        !config.naming.enum_pattern.empty() ||
        !config.naming.parameter_pattern.empty() ||
        !config.naming.localparam_pattern.empty() ||
        config.naming.check_module_filename ||
        config.naming.check_package_filename);
    if (!config.statement.case_missing_default && !config.function.functions_automatic &&
        !config.function.explicit_function_lifetime && !config.function.explicit_task_lifetime &&
        config.module.module_instantiation_style.empty() && !config.module.one_module_per_file &&
        !config.module.stale_autoinst_diagnostic && !config.statement.latch_inference_detection &&
        !config.statement.explicit_begin && !config.statement.no_raw_always &&
        !config.statement.blocking_nonblocking_assignments && config.naming.register_pattern.empty() &&
        !naming_active)
        return diags;

    auto& sm = state.tree->sourceManager();
    LintVisitor v(config, merged_index ? *merged_index : state.index, sm, file_stem_from_uri(state.uri));
    state.tree->root().visit(v);
    diags.insert(diags.end(), v.diags.begin(), v.diags.end());
    return diags;
}
