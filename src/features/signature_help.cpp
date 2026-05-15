#include "signature_help.hpp"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <memory>
#include <slang/syntax/AllSyntax.h>
#include <slang/syntax/SyntaxTree.h>
#include <slang/syntax/SyntaxVisitor.h>
#include <variant>

using namespace slang;
using namespace slang::syntax;

namespace {

struct CallContext {
    std::string name;
    std::variant<int, std::string> active{0};
    bool is_module_param{false};
};

struct ParamInfo {
    std::string name;
    std::string direction;
    std::string type;
    std::string default_value;
};

struct SubroutineInfo {
    std::string kind;
    std::string return_type;
    std::vector<ParamInfo> args;
};

static std::string trim(std::string text) {
    auto first =
        std::find_if_not(text.begin(), text.end(), [](unsigned char c) { return std::isspace(c); });
    auto last = std::find_if_not(text.rbegin(), text.rend(), [](unsigned char c) {
                    return std::isspace(c);
                }).base();
    if (first >= last)
        return {};
    return std::string(first, last);
}

static std::string token_text(const parsing::Token& token) {
    return token ? std::string(token.valueText()) : std::string{};
}

static std::string name_text(const NameSyntax& name) {
    if (const auto* ident = name.as_if<IdentifierNameSyntax>())
        return token_text(ident->identifier);
    return trim(name.toString());
}

static bool is_ident_char(char c) { return std::isalnum((unsigned char)c) || c == '_' || c == '$'; }

static std::string ident_before(std::string_view text, size_t end) {
    while (end > 0 && std::isspace((unsigned char)text[end - 1]))
        --end;
    size_t start = end;
    while (start > 0 && is_ident_char(text[start - 1]))
        --start;
    if (start == end)
        return {};
    return std::string(text.substr(start, end - start));
}

static std::optional<std::string> dotted_ident_before(std::string_view text, size_t end) {
    while (end > 0 && std::isspace((unsigned char)text[end - 1]))
        --end;
    size_t name_end = end;
    size_t name_start = name_end;
    while (name_start > 0 && is_ident_char(text[name_start - 1]))
        --name_start;
    if (name_start == name_end)
        return std::nullopt;
    size_t dot = name_start;
    while (dot > 0 && std::isspace((unsigned char)text[dot - 1]))
        --dot;
    if (dot == 0 || text[dot - 1] != '.')
        return std::nullopt;
    return std::string(text.substr(name_start, name_end - name_start));
}

static std::optional<CallContext> find_call_context(std::string_view prefix) {
    int depth = 0;
    int active = 0;
    std::optional<std::string> named_port;
    for (size_t pos = prefix.size(); pos > 0; --pos) {
        const char c = prefix[pos - 1];
        if (c == ')') {
            ++depth;
        } else if (c == '(') {
            if (depth == 0) {
                const auto before = prefix.substr(0, pos - 1);
                if (auto named = dotted_ident_before(before, before.size())) {
                    if (!named_port)
                        named_port = *named;
                    continue;
                }

                size_t end = before.size();
                while (end > 0 && std::isspace((unsigned char)before[end - 1]))
                    --end;
                bool module_param = false;
                if (end > 0 && before[end - 1] == '#') {
                    module_param = true;
                    --end;
                }

                auto name = ident_before(before, end);
                if (name.empty())
                    return std::nullopt;
                return CallContext{name,
                                   named_port ? std::variant<int, std::string>(*named_port)
                                              : std::variant<int, std::string>(active),
                                   module_param};
            }
            --depth;
        } else if (c == ',' && depth == 0 && !named_port) {
            ++active;
        }
    }
    return std::nullopt;
}

static std::vector<std::pair<std::string, std::shared_ptr<const DocumentState>>>
open_states(const Analyzer& analyzer) {
    std::vector<std::pair<std::string, std::shared_ptr<const DocumentState>>> states;
    analyzer.for_each_state(
        [&](const std::string& uri, const std::shared_ptr<const DocumentState>& state) {
            states.emplace_back(uri, state);
        });
    return states;
}

static std::optional<SubroutineInfo> subroutine_from_tree(const SyntaxTree& tree,
                                                          const std::string& name) {
    struct Visitor : public SyntaxVisitor<Visitor> {
        const std::string& name;
        std::optional<SubroutineInfo> result;

        explicit Visitor(const std::string& name) : name(name) {}

        void handle(const FunctionDeclarationSyntax& node) {
            if (result)
                return;
            if (name_text(*node.prototype->name) != name)
                return;

            SubroutineInfo info;
            info.kind = node.kind == SyntaxKind::TaskDeclaration ? "task" : "function";
            if (info.kind == "function")
                info.return_type = trim(node.prototype->returnType->toString());

            if (node.prototype->portList) {
                for (const auto* port_base : node.prototype->portList->ports) {
                    const auto* port = port_base ? port_base->as_if<FunctionPortSyntax>() : nullptr;
                    if (!port || !port->declarator)
                        continue;
                    ParamInfo param;
                    param.name = token_text(port->declarator->name);
                    param.direction = token_text(port->direction);
                    if (port->dataType)
                        param.type = trim(port->dataType->toString());
                    if (port->declarator->initializer)
                        param.default_value = trim(port->declarator->initializer->expr->toString());
                    if (!param.name.empty())
                        info.args.push_back(std::move(param));
                }
            }
            result = std::move(info);
        }
    };

    Visitor visitor(name);
    tree.root().visit(visitor);
    return visitor.result;
}

static std::optional<std::vector<ParamInfo>> module_params_from_tree(const SyntaxTree& tree,
                                                                     const std::string& name) {
    struct Visitor : public SyntaxVisitor<Visitor> {
        const std::string& name;
        std::optional<std::vector<ParamInfo>> result;

        explicit Visitor(const std::string& name) : name(name) {}

        void handle(const ModuleDeclarationSyntax& node) {
            if (result || token_text(node.header->name) != name)
                return;
            std::vector<ParamInfo> params;
            if (node.header->parameters) {
                for (const auto* base : node.header->parameters->declarations) {
                    const auto* parameter =
                        base ? base->as_if<ParameterDeclarationSyntax>() : nullptr;
                    if (!parameter)
                        continue;
                    const auto type = trim(parameter->type->toString());
                    for (const auto* decl : parameter->declarators) {
                        if (!decl)
                            continue;
                        ParamInfo param;
                        param.name = token_text(decl->name);
                        param.type = type;
                        if (!param.name.empty())
                            params.push_back(std::move(param));
                    }
                }
            }
            if (!params.empty())
                result = std::move(params);
        }
    };

    Visitor visitor(name);
    tree.root().visit(visitor);
    return visitor.result;
}

static std::optional<SubroutineInfo> find_subroutine(const Analyzer& analyzer,
                                                     const std::string& name) {
    for (const auto& [uri, state] : open_states(analyzer)) {
        (void)uri;
        if (state && state->tree) {
            if (auto found = subroutine_from_tree(*state->tree, name))
                return found;
        }
    }
    for (const auto& extra : analyzer.extra_file_snapshots()) {
        if (!extra.state || !extra.state->tree)
            continue;
        if (auto found = subroutine_from_tree(*extra.state->tree, name))
            return found;
    }
    return std::nullopt;
}

static std::optional<std::vector<ParamInfo>> find_module_params(const Analyzer& analyzer,
                                                                const std::string& name) {
    for (const auto& [uri, state] : open_states(analyzer)) {
        (void)uri;
        if (state && state->tree) {
            if (auto found = module_params_from_tree(*state->tree, name))
                return found;
        }
    }
    for (const auto& extra : analyzer.extra_file_snapshots()) {
        if (!extra.state || !extra.state->tree)
            continue;
        if (auto found = module_params_from_tree(*extra.state->tree, name))
            return found;
    }
    return std::nullopt;
}

static std::string format_arg(const ParamInfo& param) {
    std::vector<std::string> parts;
    if (!param.direction.empty())
        parts.push_back(param.direction);
    if (!param.type.empty() && param.type != "void")
        parts.push_back(param.type);
    parts.push_back(param.name);
    if (!param.default_value.empty())
        parts.push_back("= " + param.default_value);

    std::string out;
    for (const auto& part : parts) {
        if (!out.empty())
            out += ' ';
        out += part;
    }
    return out;
}

static int resolve_active(const std::variant<int, std::string>& active,
                          const std::vector<ParamInfo>& params) {
    if (std::holds_alternative<int>(active))
        return std::get<int>(active);
    const auto& name = std::get<std::string>(active);
    for (size_t i = 0; i < params.size(); ++i) {
        if (params[i].name == name)
            return (int)i;
    }
    return 0;
}

static lsSignatureHelp make_help(const std::string& label, const std::vector<std::string>& parts,
                                 int active) {
    lsSignatureInformation sig;
    sig.label = label;
    for (const auto& part : parts) {
        lsParameterInformation p;
        p.label = part;
        sig.parameters.push_back(std::move(p));
    }

    lsSignatureHelp help;
    help.signatures.push_back(std::move(sig));
    help.activeSignature = 0;
    help.activeParameter = active;
    return help;
}

} // namespace

std::optional<lsSignatureHelp> provide_signature_help(const Analyzer& analyzer,
                                                      const lsTextDocumentPositionParams& params) {
    auto state = analyzer.get_state(params.textDocument.uri.raw_uri_);
    if (!state || !state->tree)
        return std::nullopt;

    std::string prefix;
    int current_line = 0;
    size_t line_start = 0;
    for (size_t i = 0; i < state->text.size() && current_line < params.position.line; ++i) {
        if (state->text[i] == '\n') {
            ++current_line;
            line_start = i + 1;
        }
    }
    if (current_line != params.position.line)
        return std::nullopt;
    const size_t cursor =
        std::min(line_start + (size_t)params.position.character, state->text.size());
    prefix = state->text.substr(0, cursor);

    auto ctx = find_call_context(prefix);
    if (!ctx)
        return std::nullopt;

    if (ctx->is_module_param) {
        auto params_info = find_module_params(analyzer, ctx->name);
        if (!params_info)
            return std::nullopt;
        std::vector<std::string> labels;
        for (const auto& param : *params_info)
            labels.push_back(format_arg(param));
        int active = std::min(resolve_active(ctx->active, *params_info),
                              std::max((int)params_info->size() - 1, 0));
        return make_help(
            "module " + ctx->name + " #(" +
                [&] {
                    std::string joined;
                    for (size_t i = 0; i < labels.size(); ++i) {
                        if (i)
                            joined += ", ";
                        joined += labels[i];
                    }
                    return joined;
                }() +
                ")",
            labels, active);
    }

    auto subroutine = find_subroutine(analyzer, ctx->name);
    if (!subroutine) {
        auto params_info = find_module_params(analyzer, ctx->name);
        if (!params_info)
            return std::nullopt;
        std::vector<std::string> labels;
        for (const auto& param : *params_info)
            labels.push_back(format_arg(param));
        int active = std::min(resolve_active(ctx->active, *params_info),
                              std::max((int)params_info->size() - 1, 0));
        std::string joined;
        for (size_t i = 0; i < labels.size(); ++i) {
            if (i)
                joined += ", ";
            joined += labels[i];
        }
        return make_help("module " + ctx->name + " #(" + joined + ")", labels, active);
    }

    std::vector<std::string> labels;
    for (const auto& arg : subroutine->args)
        labels.push_back(format_arg(arg));

    std::string prefix_label;
    if (subroutine->kind == "function" && !subroutine->return_type.empty() &&
        subroutine->return_type != "void")
        prefix_label = "function " + subroutine->return_type + " ";
    else if (subroutine->kind == "task")
        prefix_label = "task ";

    std::string joined;
    for (size_t i = 0; i < labels.size(); ++i) {
        if (i)
            joined += ", ";
        joined += labels[i];
    }
    int active = std::min(resolve_active(ctx->active, subroutine->args),
                          std::max((int)subroutine->args.size() - 1, 0));
    return make_help(prefix_label + ctx->name + "(" + joined + ")", labels, active);
}
