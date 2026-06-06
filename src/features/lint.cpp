#include "lint.hpp"
#include "../analyzer.hpp"
#include "../dynamic_file_index.hpp"
#include <slang/syntax/AllSyntax.h>
#include <slang/syntax/SyntaxTree.h>
#include <slang/syntax/SyntaxVisitor.h>
#include <slang/text/SourceManager.h>
#include <slang/parsing/TokenKind.h>
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <memory>
#include <regex>
#include <optional>
#include <string_view>
#include <unordered_map>
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
        try {
            auto file_name = sm.getFileName(loc);
            if (!file_name.empty()) {
                d.uri = std::string(file_name);
                if (!d.uri.starts_with("file://"))
                    d.uri = "file://" + std::filesystem::absolute(d.uri).lexically_normal().string();
            }
        } catch (const std::exception& e) {
            // URI attribution failures should not drop the diagnostic, but they
            // also should not be invisible: clients will fall back to the
            // owning document when d.uri is empty, and this log explains why an
            // included-file diagnostic may not carry its original URI.
            std::cerr << "[lazyverilog] lint diagnostic URI resolution failed: "
                      << e.what() << "\n";
        } catch (...) {
            std::cerr << "[lazyverilog] lint diagnostic URI resolution failed: "
                         "non-standard exception\n";
        }
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

using CachedRegex = std::shared_ptr<const std::regex>;

static CachedRegex compile_re(const std::string& pat) {
    if (pat.empty())
        return nullptr;

    // Naming lint runs on every diagnostics refresh.  Keep compilation
    // thread-local and keyed by the exact config pattern so repeated didChange
    // notifications pay only a hash lookup, while config edits naturally
    // compile a new pattern the first time it is observed on this worker.
    //
    // Invalid patterns are cached as nullptr too.  That preserves the previous
    // "silently disable invalid naming rule" behavior without repeatedly
    // throwing std::regex_error on every keystroke.
    thread_local std::unordered_map<std::string, CachedRegex> cache;
    if (auto it = cache.find(pat); it != cache.end())
        return it->second;

    CachedRegex compiled;
    try {
        compiled = std::make_shared<const std::regex>(pat);
    } catch (...) {
        compiled = nullptr;
    }
    cache.emplace(pat, compiled);
    return compiled;
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

struct CurrentModulePorts {
    std::vector<std::string> ports;
    std::unordered_set<std::string> port_by_name;

    void add(std::string name) {
        if (name.empty() || !port_by_name.insert(name).second)
            return;
        ports.push_back(std::move(name));
    }
};

using CurrentModulePortMap = std::unordered_map<std::string, CurrentModulePorts>;

struct CurrentModulePortCollector : public SyntaxVisitor<CurrentModulePortCollector> {
    CurrentModulePortMap modules;

    void add_ansi_ports(const AnsiPortListSyntax* port_list, CurrentModulePorts& out) {
        if (!port_list)
            return;
        for (const auto* member : port_list->ports) {
            if (!member)
                continue;
            if (const auto* implicit = member->as_if<ImplicitAnsiPortSyntax>()) {
                if (implicit->declarator && implicit->declarator->name)
                    out.add(std::string(implicit->declarator->name.valueText()));
            } else if (const auto* explicit_port = member->as_if<ExplicitAnsiPortSyntax>()) {
                if (explicit_port->name)
                    out.add(std::string(explicit_port->name.valueText()));
            }
        }
    }

    void add_port_declarations(const SyntaxList<MemberSyntax>& members, CurrentModulePorts& out) {
        for (const auto* member : members) {
            if (!member)
                continue;
            const auto* declaration = member->as_if<PortDeclarationSyntax>();
            if (!declaration)
                continue;
            for (const auto* declarator : declaration->declarators) {
                if (declarator && declarator->name)
                    out.add(std::string(declarator->name.valueText()));
            }
        }
    }

    void handle(const ModuleDeclarationSyntax& node) {
        if (!node.header || !node.header->name)
            return;

        CurrentModulePorts ports;
        if (node.header->ports && node.header->ports->kind == SyntaxKind::AnsiPortList)
            add_ansi_ports(node.header->ports->as_if<AnsiPortListSyntax>(), ports);
        add_port_declarations(node.members, ports);
        modules[std::string(node.header->name.valueText())] = std::move(ports);

        // Do not call visitDefault(node): SystemVerilog modules are not nested,
        // and this prepass only needs top-level module declarations from the
        // current live AST snapshot.  Keeping it shallow avoids extra work on
        // every diagnostics refresh.
    }
};

struct LintVisitor : public SyntaxVisitor<LintVisitor> {
    const LintConfig&          cfg;
    const CurrentModulePortMap& current_modules;
    const SyntaxIndex*         project_index;
    const ProjectIndexSnapshot* project_snapshot;
    SourceManager&             sm;
    std::vector<ParseDiagInfo> diags;
    bool in_always_ff_{false};
    bool in_always_comb_{false};
    int module_count_{0};
    std::string file_stem_;

    // Shared pointers into a thread-local pattern cache.  Copying the pointer is
    // cheap; compiling std::regex on every lint pass is not.
    CachedRegex module_re_;
    CachedRegex input_port_re_;
    CachedRegex output_port_re_;
    CachedRegex signal_re_;
    CachedRegex register_re_;
    CachedRegex interface_re_;
    CachedRegex struct_re_;
    CachedRegex union_re_;
    CachedRegex enum_re_;
    CachedRegex parameter_re_;
    CachedRegex localparam_re_;

    LintVisitor(const LintConfig& c, const CurrentModulePortMap& current,
                const SyntaxIndex* project, const ProjectIndexSnapshot* snapshot,
                SourceManager& s, std::string file_stem)
        : cfg(c), current_modules(current), project_index(project), project_snapshot(snapshot), sm(s),
          file_stem_(std::move(file_stem)) {
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
                  const CachedRegex& re,
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

    // ── function_call_style: "named" | "positional" | "both" ─────────────────
    void handle(const InvocationExpressionSyntax& node) {
        const auto& style = cfg.function.function_call_style;
        if (!style.empty() && node.arguments) {
            bool has_positional = false, has_named = false;
            for (auto* arg : node.arguments->parameters) {
                if (!arg) continue;
                if (arg->as_if<OrderedArgumentSyntax>())
                    has_positional = true;
                else if (arg->as_if<NamedArgumentSyntax>())
                    has_named = true;
            }
            auto fn_sev = cfg.function.enable ? (cfg.function.severity == "error"   ? 1
                                                 : cfg.function.severity == "hint"   ? 3
                                                                                      : 2)
                                              : 2;
            if (style == "named" && has_positional)
                diags.push_back(make_diag(sm, node.left->getFirstToken().location(), fn_sev,
                    "[function] call uses positional arguments; named arguments required"));
            else if (style == "positional" && has_named)
                diags.push_back(make_diag(sm, node.left->getFirstToken().location(), fn_sev,
                    "[function] call uses named arguments; positional arguments required"));
            else if (style == "both" && has_positional && has_named)
                diags.push_back(make_diag(sm, node.left->getFirstToken().location(), fn_sev,
                    "[function] call mixes positional and named arguments"));
        }
        visitDefault(node);
    }

    const CurrentModulePorts* find_current_module_ports(std::string_view module_name) const {
        const auto it = current_modules.find(std::string(module_name));
        return it == current_modules.end() ? nullptr : &it->second;
    }

    const ModuleEntry* find_project_module_entry(std::string_view module_name) const {
        const std::string key(module_name);
        if (project_snapshot) {
            const auto it = project_snapshot->module_by_name.find(key);
            if (it == project_snapshot->module_by_name.end() || !it->second.shard ||
                it->second.module_index >= it->second.shard->modules.size())
                return nullptr;
            return &it->second.shard->modules[it->second.module_index];
        }
        if (!project_index)
            return nullptr;
        const auto it = project_index->module_by_name.find(key);
        if (it == project_index->module_by_name.end() || it->second >= project_index->modules.size())
            return nullptr;
        return &project_index->modules[it->second];
    }

    template <typename HasPort, typename ForEachPort>
    void check_stale_autoinst_connections(const HierarchyInstantiationSyntax& node,
                                          HasPort&& has_port,
                                          ForEachPort&& for_each_port) {
        for (uint32_t i = 0; i < node.instances.size(); ++i) {
            const auto* inst = node.instances[i];
            if (!inst)
                continue;
            std::unordered_set<std::string> seen;
            for (uint32_t j = 0; j < inst->connections.size(); ++j) {
                const auto* named = inst->connections[j]
                                        ? inst->connections[j]->as_if<NamedPortConnectionSyntax>()
                                        : nullptr;
                if (!named)
                    continue;
                std::string port = std::string(named->name.valueText());
                if (!seen.insert(port).second)
                    diags.push_back(make_diag(sm, named->name.location(), module_sev(),
                        "[module] duplicate autoinst connection for port '" + port + "'"));
                else if (!has_port(port))
                    diags.push_back(make_diag(sm, named->name.location(), module_sev(),
                        "[module] stale autoinst connection for unknown port '" + port + "'"));
            }
            for_each_port([&](const std::string& port_name) {
                if (!seen.count(port_name) && inst->decl)
                    diags.push_back(make_diag(sm, inst->decl->name.location(), module_sev(),
                        "[module] autoinst connection missing port '" + port_name + "'"));
            });
        }
    }

    // ── module_instantiation_style: "named" | "positional" | "both" ─────────
    void handle(const HierarchyInstantiationSyntax& node) {
        const auto& style = cfg.module.module_instantiation_style;
        if (!style.empty()) {
            for (uint32_t i = 0; i < node.instances.size(); ++i) {
                const auto* inst = node.instances[i];
                if (!inst) continue;
                bool has_positional = false, has_named = false;
                for (uint32_t j = 0; j < inst->connections.size(); ++j) {
                    if (const auto* conn = inst->connections[j]) {
                        if (conn->as_if<OrderedPortConnectionSyntax>())
                            has_positional = true;
                        else if (conn->as_if<NamedPortConnectionSyntax>())
                            has_named = true;
                    }
                }
                if (style == "named" && has_positional)
                    diags.push_back(make_diag(sm, node.type.location(), module_sev(),
                        "[module] instance uses positional port connections; named connections required"));
                else if (style == "positional" && has_named)
                    diags.push_back(make_diag(sm, node.type.location(), module_sev(),
                        "[module] instance uses named port connections; positional connections required"));
                else if (style == "both" && has_positional && has_named)
                    diags.push_back(make_diag(sm, node.type.location(), module_sev(),
                        "[module] instance mixes positional and named port connections"));
            }
        }
        if (cfg.module.stale_autoinst_diagnostic) {
            const auto module_name = std::string_view(node.type.valueText());
            if (const auto* current = find_current_module_ports(module_name)) {
                // Current/open-file facts come from the live AST.  They win over
                // the project index so unsaved edits are reflected immediately.
                check_stale_autoinst_connections(
                    node,
                    [&](const std::string& port) { return current->port_by_name.count(port) != 0; },
                    [&](auto&& fn) {
                        for (const auto& port : current->ports)
                            fn(port);
                    });
            } else if (const auto* project = find_project_module_entry(module_name)) {
                // Closed/project-file facts come from the immutable background
                // index snapshot.  Request-time diagnostics do not build or
                // merge project-wide indexes.
                check_stale_autoinst_connections(
                    node,
                    [&](const std::string& port) { return project->port_by_name.count(port) != 0; },
                    [&](auto&& fn) {
                        for (const auto& port : project->ports)
                            fn(port.name);
                    });
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

static std::vector<ParseDiagInfo> run_lint_impl(const DocumentState& state, const LintConfig& config,
                                                const SyntaxIndex* project_index,
                                                const ProjectIndexSnapshot* project_snapshot) {
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
        config.module.module_instantiation_style.empty() && config.function.function_call_style.empty() &&
        !config.module.one_module_per_file &&
        !config.module.stale_autoinst_diagnostic && !config.statement.latch_inference_detection &&
        !config.statement.explicit_begin && !config.statement.no_raw_always &&
        !config.statement.blocking_nonblocking_assignments && config.naming.register_pattern.empty() &&
        !naming_active)
        return diags;

    auto& sm = state.tree->sourceManager();
    // Most lint rules are pure SyntaxTree walks.  Do not synthesize a broad
    // file index just because lint is running on didChange; that puts indexing
    // back on the editing hot path.  The stale-autoinst rule needs module port
    // declarations, but current/open-file facts are derived from the live AST
    // below while closed/project facts are read from project_index.
    CurrentModulePortMap current_modules;
    if (config.module.stale_autoinst_diagnostic) {
        CurrentModulePortCollector collector;
        state.tree->root().visit(collector);
        current_modules = std::move(collector.modules);
    }
    LintVisitor v(config, current_modules, project_index, project_snapshot, sm, file_stem_from_uri(state.uri));
    state.tree->root().visit(v);
    v.diags.erase(std::remove_if(v.diags.begin(), v.diags.end(), [&](const auto& diag) {
        return !diag.uri.empty() && diag.uri != state.uri;
    }), v.diags.end());
    diags.insert(diags.end(), v.diags.begin(), v.diags.end());
    return diags;
}

std::vector<ParseDiagInfo> run_lint(const DocumentState& state, const LintConfig& config,
                                    const SyntaxIndex* project_index) {
    return run_lint_impl(state, config, project_index, nullptr);
}

std::vector<ParseDiagInfo> run_lint(const DocumentState& state, const LintConfig& config,
                                    const ProjectIndexSnapshot* project_index) {
    return run_lint_impl(state, config, nullptr, project_index);
}
