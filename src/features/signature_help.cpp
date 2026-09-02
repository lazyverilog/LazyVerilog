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
    // Offset of the callee name in the prefix, and whether a `::` qualifier
    // precedes it.  A qualified callee names one specific subroutine, so it must
    // not be matched by bare name against the whole file.
    size_t name_offset{0};
    bool is_scoped{false};
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

static bool same_signature(const SubroutineInfo& lhs, const SubroutineInfo& rhs) {
    if (lhs.kind != rhs.kind || lhs.return_type != rhs.return_type ||
        lhs.args.size() != rhs.args.size())
        return false;
    for (size_t i = 0; i < lhs.args.size(); ++i) {
        const auto& a = lhs.args[i];
        const auto& b = rhs.args[i];
        if (a.name != b.name || a.direction != b.direction || a.type != b.type ||
            a.default_value != b.default_value)
            return false;
    }
    return true;
}

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
                size_t name_end = end;
                while (name_end > 0 && std::isspace((unsigned char)before[name_end - 1]))
                    --name_end;
                const size_t name_start = name_end - name.size();
                const bool scoped =
                    name_start >= 2 && before.compare(name_start - 2, 2, "::") == 0;
                return CallContext{name,
                                   named_port ? std::variant<int, std::string>(*named_port)
                                              : std::variant<int, std::string>(active),
                                   module_param, name_start, scoped};
            }
            --depth;
        } else if (c == ',' && depth == 0 && !named_port) {
            ++active;
        }
    }
    return std::nullopt;
}

static SubroutineInfo subroutine_info_from_declaration(const FunctionDeclarationSyntax& node) {
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
    return info;
}

static std::optional<SubroutineInfo> subroutine_from_tree(const SyntaxTree& tree,
                                                          const std::string& name) {
    struct Visitor : public SyntaxVisitor<Visitor> {
        const std::string& name;
        std::optional<SubroutineInfo> result;
        // Every class declares its own `new`, so a bare-name match would answer
        // a constructor call with whichever class happens to come first in the
        // file.  Which one a call means depends on the type being constructed,
        // which this text-driven provider cannot resolve.  Candidates that all
        // render the same signature are still safe to show -- UVM's factory
        // macros give every class an identical `create`, for instance -- so
        // only genuinely differing candidates make the name ambiguous.
        bool ambiguous{false};

        explicit Visitor(const std::string& name) : name(name) {}

        void handle(const FunctionDeclarationSyntax& node) {
            if (ambiguous || name_text(*node.prototype->name) != name)
                return;

            SubroutineInfo info = subroutine_info_from_declaration(node);

            if (result && !same_signature(*result, info)) {
                ambiguous = true;
                result.reset();
                return;
            }
            result = std::move(info);
        }
    };

    Visitor visitor(name);
    tree.root().visit(visitor);
    return visitor.result;
}

/// The subroutine declared at @p line / @p col (both 0-based), if any.
///
/// Used for `::`-qualified callees, where the name alone is ambiguous but
/// go-to-definition already knows which declaration the call means.
static std::optional<SubroutineInfo> subroutine_at_declaration(const SyntaxTree& tree, int line,
                                                                int col) {
    struct Visitor : public SyntaxVisitor<Visitor> {
        const SourceManager& sm;
        int line;
        int col;
        std::optional<SubroutineInfo> result;

        Visitor(const SourceManager& sm, int line, int col) : sm(sm), line(line), col(col) {}

        void handle(const FunctionDeclarationSyntax& node) {
            if (result)
                return;
            const auto token = node.prototype->name->getFirstToken();
            if (!token)
                return;
            const auto loc = token.location();
            if (!loc.valid())
                return;
            if ((int)sm.getLineNumber(loc) - 1 != line || (int)sm.getColumnNumber(loc) - 1 != col)
                return;
            result = subroutine_info_from_declaration(node);
        }
    };

    Visitor visitor(tree.sourceManager(), line, col);
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

static std::optional<std::vector<ParamInfo>> module_params_from_module(const ModuleEntry& module) {
    std::vector<ParamInfo> params;
    for (const auto& port : module.ports) {
        if (port.direction != "parameter" && port.direction != "localparam")
            continue;
        params.push_back(ParamInfo{.name = port.name,
                                   .direction = port.direction,
                                   .type = port.type,
                                   .default_value = port.default_value});
    }
    if (params.empty())
        return std::nullopt;
    return params;
}

static std::optional<std::vector<ParamInfo>> module_params_from_snapshot(
    const ProjectIndexSnapshot& index, const std::string& name) {
    const auto it = index.module_by_name.find(name);
    if (it == index.module_by_name.end() || !it->second.shard ||
        it->second.module_index >= it->second.shard->modules.size())
        return std::nullopt;
    return module_params_from_module(it->second.shard->modules[it->second.module_index]);
}

static std::optional<std::vector<ParamInfo>> find_module_params(const Analyzer& analyzer,
                                                                const SyntaxTree& current_tree,
                                                                const std::string& name) {
    if (auto found = module_params_from_tree(current_tree, name))
        return found;
    if (auto project = analyzer.project_index_snapshot())
        return module_params_from_snapshot(*project, name);
    return std::nullopt;
}

// ── Subroutines declared outside the current document ────────────────────────
//
// A call to a function in an imported package is the ordinary case, and the
// declaring file is usually closed.  Closed project files deliberately keep no
// AST (see the index architecture rules), so the shard's pre-rendered
// `signature` is the only description of that declaration available here.
// Recover the structured parameter list from it rather than re-parsing the file.

/// Split on commas that sit outside any bracket group, so a parameterized type
/// such as `my_cls#(1,2) handle` stays one argument.
static std::vector<std::string> split_top_level_commas(std::string_view text) {
    std::vector<std::string> parts;
    int depth = 0;
    size_t start = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '(' || c == '[' || c == '{')
            ++depth;
        else if (c == ')' || c == ']' || c == '}')
            --depth;
        else if (c == ',' && depth == 0) {
            parts.push_back(std::string(text.substr(start, i - start)));
            start = i + 1;
        }
    }
    parts.push_back(std::string(text.substr(start)));
    return parts;
}

static std::string trim_ws(std::string_view text) {
    size_t b = 0, e = text.size();
    while (b < e && std::isspace((unsigned char)text[b]))
        ++b;
    while (e > b && std::isspace((unsigned char)text[e - 1]))
        --e;
    return std::string(text.substr(b, e - b));
}

/// `input bit [7:0] addr = 8'h0` -> direction/type/name/default.
///
/// The declared name is the last identifier before any unpacked dimensions, so
/// the split is done from the right; everything left of it is the direction and
/// type exactly as the source spelled them.
static ParamInfo parse_arg_text(const std::string& raw) {
    ParamInfo info;
    std::string text = trim_ws(raw);

    // Default value first: everything after a top-level '='.
    int depth = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '(' || c == '[' || c == '{')
            ++depth;
        else if (c == ')' || c == ']' || c == '}')
            --depth;
        else if (c == '=' && depth == 0) {
            info.default_value = trim_ws(std::string_view(text).substr(i + 1));
            text = trim_ws(std::string_view(text).substr(0, i));
            break;
        }
    }

    // Trailing unpacked dimensions belong to the type, not the name.
    std::string trailing_dims;
    while (!text.empty() && text.back() == ']') {
        const size_t open = text.rfind('[');
        if (open == std::string::npos)
            break;
        trailing_dims = text.substr(open) + trailing_dims;
        text = trim_ws(std::string_view(text).substr(0, open));
    }

    size_t end = text.size();
    while (end > 0 && is_ident_char(text[end - 1]))
        --end;
    info.name = text.substr(end);
    std::string lead = trim_ws(std::string_view(text).substr(0, end));

    static const char* kDirections[] = {"input", "output", "inout", "ref", nullptr};
    for (int i = 0; kDirections[i]; ++i) {
        const std::string dir = kDirections[i];
        if (lead.rfind(dir, 0) == 0 &&
            (lead.size() == dir.size() || std::isspace((unsigned char)lead[dir.size()]))) {
            info.direction = dir;
            lead = trim_ws(std::string_view(lead).substr(dir.size()));
            break;
        }
    }
    info.type = lead + trailing_dims;
    return info;
}

/// Rebuild a SubroutineInfo from the shard's rendered signature, which
/// make_subroutine_signature() produced as
/// "```\n[function <ret>|task] <name>(<ports>)\n```".
static std::optional<SubroutineInfo> subroutine_from_index_signature(const std::string& signature,
                                                                      const std::string& name) {
    std::string body = signature;
    const std::string fence = "```";
    if (body.rfind(fence, 0) == 0)
        body = body.substr(fence.size());
    if (body.size() >= fence.size() && body.compare(body.size() - fence.size(), fence.size(),
                                                    fence) == 0)
        body = body.substr(0, body.size() - fence.size());
    // The port list is rendered one argument per line for readability; commas
    // still separate them, so newlines can collapse to spaces.
    std::replace(body.begin(), body.end(), '\n', ' ');
    body = trim_ws(body);

    const size_t open = body.find('(');
    std::string header = trim_ws(open == std::string::npos ? body : body.substr(0, open));
    std::string ports;
    if (open != std::string::npos) {
        const size_t close = body.rfind(')');
        if (close == std::string::npos || close < open)
            return std::nullopt;
        ports = trim_ws(body.substr(open + 1, close - open - 1));
    }

    SubroutineInfo info;
    if (header.rfind("function", 0) == 0)
        info.kind = "function";
    else if (header.rfind("task", 0) == 0)
        info.kind = "task";
    else
        return std::nullopt;

    // Strip the keyword and the trailing declared name; what remains is the
    // return type ("" for a task).
    header = trim_ws(std::string_view(header).substr(info.kind.size()));
    if (header.size() >= name.size() &&
        header.compare(header.size() - name.size(), name.size(), name) == 0)
        header = trim_ws(std::string_view(header).substr(0, header.size() - name.size()));
    info.return_type = header;

    if (!ports.empty()) {
        for (const auto& part : split_top_level_commas(ports)) {
            const std::string trimmed = trim_ws(part);
            if (!trimmed.empty())
                info.args.push_back(parse_arg_text(trimmed));
        }
    }
    return info;
}

/// The one subroutine named @p name across a shard, or nothing when the shard
/// holds several that would render differently.  Same discipline as the
/// current-file visitor: an ambiguous name is not worth a guess.
static void collect_subroutine_from_shard(const SyntaxIndex& index, const std::string& name,
                                          std::optional<SubroutineInfo>& result,
                                          bool& ambiguous) {
    for (const auto& value : index.values) {
        if (ambiguous)
            return;
        if (value.name != name || value.signature.empty())
            continue;
        if (value.kind != "function" && value.kind != "task")
            continue;
        auto parsed = subroutine_from_index_signature(value.signature, name);
        if (!parsed)
            continue;
        if (!result)
            result = std::move(parsed);
        else if (!same_signature(*result, *parsed))
            ambiguous = true;
    }
}

static std::optional<SubroutineInfo> find_subroutine_outside_document(const Analyzer& analyzer,
                                                                      const std::string& uri,
                                                                      const std::string& name) {
    std::optional<SubroutineInfo> result;
    bool ambiguous = false;
    if (auto opened = analyzer.opened_file_index_shards(uri)) {
        for (const auto& shard : *opened) {
            if (shard.index)
                collect_subroutine_from_shard(*shard.index, name, result, ambiguous);
        }
    }
    if (auto project = analyzer.project_index_snapshot()) {
        for (const auto& shard : project->shards) {
            if (shard.index)
                collect_subroutine_from_shard(*shard.index, name, result, ambiguous);
        }
    }
    if (ambiguous)
        return std::nullopt;
    return result;
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
        auto params_info = find_module_params(analyzer, *state->tree, ctx->name);
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

    std::optional<SubroutineInfo> subroutine;
    if (ctx->is_scoped) {
        // `my_item::type_id::create(...)` names one declaration, and the class
        // the call is written on usually has a same-named method of its own --
        // UVM's factory macros give every class a `create`.  Ask go-to-definition
        // which one is meant instead of matching the bare name.  A declaration
        // outside this document has no parameter list to show, so returning
        // nothing there is the honest answer.
        if (auto def = analyzer.definition_of(params.textDocument.uri.raw_uri_,
                                              params.position.line,
                                              (int)(ctx->name_offset - line_start))) {
            if (def->uri == params.textDocument.uri.raw_uri_)
                subroutine = subroutine_at_declaration(*state->tree, def->line, def->col);
        }
        if (!subroutine)
            return std::nullopt;
    } else {
        subroutine = subroutine_from_tree(*state->tree, ctx->name);
        // A call to a function imported from a package is normally a call into
        // another file, so the current document's AST is only the first place
        // to look.  Hover and go-to-definition already resolve these; signature
        // help asking the same shards keeps the three consistent.
        if (!subroutine)
            subroutine = find_subroutine_outside_document(
                analyzer, params.textDocument.uri.raw_uri_, ctx->name);
    }

    if (!subroutine) {
        auto params_info = find_module_params(analyzer, *state->tree, ctx->name);
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
