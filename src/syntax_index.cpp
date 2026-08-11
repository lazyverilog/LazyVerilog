#include "syntax_index.hpp"
#include "syntax_index_shared.hpp"
#include "string_utils.hpp"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <optional>
#include <slang/syntax/AllSyntax.h>
#include <slang/syntax/SyntaxTree.h>
#include <slang/syntax/SyntaxVisitor.h>
#include <slang/text/SourceManager.h>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

using namespace slang;
using namespace slang::syntax;

static std::string make_fn_signature(const FunctionPrototypeSyntax& proto,
                                     const std::string& name,
                                     const slang::SourceManager& sm) {
    const bool is_task =
        proto.keyword.kind == slang::parsing::TokenKind::TaskKeyword;
    const std::string ports =
        proto.portList ? trim_copy(proto.portList->toString()) : "";

    std::string formatted_ports;
    if (ports.size() >= 2 && ports.front() == '(' && ports.back() == ')') {
        auto inner = trim_copy(ports.substr(1, ports.size() - 2));
        if (inner.empty()) {
            formatted_ports = "()";
        } else {
            std::vector<std::string> parts;
            size_t start = 0;
            while (start <= inner.size()) {
                auto comma = inner.find(',', start);
                if (comma == std::string::npos) {
                    parts.push_back(trim_copy(inner.substr(start)));
                    break;
                }
                parts.push_back(trim_copy(inner.substr(start, comma - start)));
                start = comma + 1;
            }
            if (parts.size() <= 1) {
                formatted_ports = "(" + inner + ")";
            } else {
                formatted_ports = "(\n";
                for (size_t i = 0; i < parts.size(); ++i) {
                    formatted_ports += "    " + parts[i];
                    if (i + 1 != parts.size())
                        formatted_ports += ",\n";
                }
                formatted_ports += "\n)";
            }
        }
    } else {
        formatted_ports = ports;
    }

    if (is_task)
        return "```\ntask " + name + formatted_ports + "\n```";
    const std::string ret = render_syntax_node_text(sm, *proto.returnType);
    return "```\nfunction " + ret + " " + name + formatted_ports + "\n```";
}

static std::pair<int, int> source_range_lines(const slang::SourceManager& sm,
                                              slang::SourceRange range) {
    if (!range.start().valid() || !range.end().valid())
        return {0, 0};
    const auto start = sm.getLineNumber(range.start());
    const auto end = sm.getLineNumber(range.end());
    return {start > 0 ? (int)start : 0, end > 0 ? (int)end : 0};
}

static bool index_macro_has_user_source_location(const slang::SourceManager& sm,
                                                 const slang::parsing::Token& name) {
    // slang predefines a handful of implementation / SV coverage macros in the
    // preprocessor itself.  Those names have SourceLocation::NoLocation because
    // there is no user file to navigate to:
    //
    //     DEFINE("SV_COV_ERROR"sv, -1);
    //
    // LazyVerilog should not surface such parser implementation details as
    // project macros.
    return name && name.location().valid() && sm.isFileLoc(name.location());
}

static MacroEntry macro_entry_from_define(SyntaxIndex& index, SourceFileIdResolver& resolver,
                                          const slang::SourceManager& sm,
                                          const DefineDirectiveSyntax& def) {
    MacroEntry mac;
    mac.name = std::string(def.name.valueText());
    mac.file_id = resolver.for_token(index, sm, def.name);
    if (def.formalArguments) {
        mac.is_function_like = true;
        for (const auto* arg : def.formalArguments->args) {
            if (arg)
                mac.params.push_back(std::string(arg->name.valueText()));
        }
    }
    mac.line = token_pos_line1_col0(sm, def.name).first;
    return mac;
}

static std::string direction_of(const PortHeaderSyntax& header) {
    if (const auto* variable = header.as_if<VariablePortHeaderSyntax>())
        return token_value_text(variable->direction).empty() ? "unknown" : token_value_text(variable->direction);
    if (const auto* net = header.as_if<NetPortHeaderSyntax>())
        return token_value_text(net->direction).empty() ? "unknown" : token_value_text(net->direction);
    return "unknown";
}

static std::string type_of(const slang::SourceManager& sm, const PortHeaderSyntax& header) {
    if (const auto* variable = header.as_if<VariablePortHeaderSyntax>())
        return render_syntax_node_text(sm, *variable->dataType);
    if (const auto* net = header.as_if<NetPortHeaderSyntax>())
        return render_syntax_node_text(sm, *net->dataType);
    return {};
}

static std::string decl_type_of(const slang::SourceManager& sm, const PortHeaderSyntax& header) {
    if (const auto* variable = header.as_if<VariablePortHeaderSyntax>())
        return render_syntax_node_text(sm, *variable->dataType);
    if (const auto* net = header.as_if<NetPortHeaderSyntax>()) {
        // Net ports split the declaration into a net kind token and a data
        // type node in slang's syntax tree:
        //
        //     input wire [5:0] data
        //           ^^^^ ^^^^^
        //           |    `- net->dataType
        //           `------ net->netType
        //
        // The human-facing PortEntry::type intentionally keeps the historical
        // display text ("[5:0]").  Connect / Interface, however, need a
        // complete declaration prefix; otherwise they synthesize invalid code:
        //
        //     [5:0] data32;
        //
        // Store the full declaration type separately.  This remains syntactic
        // text: render_syntax_node_text() preserves typedef names and macro /
        // parameter based dimensions such as `WIDTH or [DEPTH-1:0].
        std::string type = token_value_text(net->netType);
        const auto data_type = render_syntax_node_text(sm, *net->dataType);
        if (!data_type.empty())
            type += (type.empty() ? "" : " ") + data_type;
        return type;
    }
    return {};
}

static std::string signal_decl_type_of(const slang::SourceManager& sm, const PortHeaderSyntax& header) {
    if (const auto* variable = header.as_if<VariablePortHeaderSyntax>())
        return render_syntax_node_text(sm, *variable->dataType);
    if (const auto* net = header.as_if<NetPortHeaderSyntax>()) {
        // This branch is selected from slang's NetPortHeaderSyntax, not from
        // textual matching.  Internal bridge signals generated by Connect /
        // Interface should be variables even when the module port is a net:
        //
        //     output wire [5:0] o_data  ->  logic [5:0] data32;
        //     output      [5:0] o_data  ->  logic [5:0] data32;
        //
        // The data type text is still rendered syntactically, so macro and
        // parameter dimensions remain as the user wrote them.
        std::string type = "logic";
        const auto data_type = render_syntax_node_text(sm, *net->dataType);
        if (!data_type.empty())
            type += " " + data_type;
        return type;
    }
    return {};
}

static std::string with_declarator_dimensions(const slang::SourceManager& sm, std::string type,
                                              const DeclaratorSyntax& declarator) {
    // A port header owns the shared packed type while each declarator owns its
    // unpacked dimensions:
    //
    //     input logic [1:0] a [7:0], b [3:0];
    //           ^^^^^^^^^^^ shared header type
    //                         ^^^^^  ^^^^^ per-declarator dimensions
    //
    // The syntax index is used by hover's fast path, so it must retain those
    // dimensions; otherwise hover only shows "input logic [1:0]".
    for (const auto* dimension : declarator.dimensions) {
        if (!dimension)
            continue;

        const auto rendered = render_syntax_node_text(sm, *dimension);
        if (rendered.empty())
            continue;

        type += (type.empty() ? "" : " ") + rendered;
    }
    return type;
}

static void add_port(std::vector<PortEntry>& ports, SyntaxIndex& index,
                     SourceFileIdResolver& resolver, const slang::SourceManager& sm,
                     const slang::parsing::Token& name, std::string direction, std::string type,
                     std::string decl_type, std::string signal_decl_type,
                     std::string default_value = {}) {
    if (!name)
        return;

    auto [line, col] = token_pos_line1_col0(sm, name);
    ports.push_back(PortEntry{
        .name = token_value_text(name),
        .file_id = resolver.for_token(index, sm, name),
        .direction = std::move(direction),
        .type = std::move(type),
        .decl_type = std::move(decl_type),
        .signal_decl_type = std::move(signal_decl_type),
        .default_value = std::move(default_value),
        .line = line,
        .col = col,
    });
}

static void extract_ansi_ports(const AnsiPortListSyntax& port_list, std::vector<PortEntry>& ports,
                               SyntaxIndex& index, SourceFileIdResolver& resolver,
                               const slang::SourceManager& sm) {
    for (const auto* member : port_list.ports) {
        if (!member)
            continue;

        if (const auto* implicit = member->as_if<ImplicitAnsiPortSyntax>()) {
            add_port(ports, index, resolver, sm, implicit->declarator->name, direction_of(*implicit->header),
                     with_declarator_dimensions(sm, type_of(sm, *implicit->header),
                                                *implicit->declarator),
                     with_declarator_dimensions(sm, decl_type_of(sm, *implicit->header),
                                                *implicit->declarator),
                     with_declarator_dimensions(sm, signal_decl_type_of(sm, *implicit->header),
                                                *implicit->declarator));
        } else if (const auto* explicit_port = member->as_if<ExplicitAnsiPortSyntax>()) {
            auto direction = token_value_text(explicit_port->direction);
            add_port(ports, index, resolver, sm, explicit_port->name,
                     direction.empty() ? std::string("unknown") : std::move(direction), {}, {}, {});
        }
    }
}

static void extract_port_declarations(const SyntaxList<MemberSyntax>& members,
                                      std::vector<PortEntry>& ports, SyntaxIndex& index,
                                      SourceFileIdResolver& resolver,
                                      const slang::SourceManager& sm) {
    for (const auto* member : members) {
        if (!member)
            continue;
        const auto* declaration = member->as_if<PortDeclarationSyntax>();
        if (!declaration)
            continue;

        const auto direction = direction_of(*declaration->header);
        const auto type = type_of(sm, *declaration->header);
        const auto decl_type = decl_type_of(sm, *declaration->header);
        const auto signal_decl_type = signal_decl_type_of(sm, *declaration->header);
        for (const auto* declarator : declaration->declarators) {
            if (declarator)
                add_port(ports, index, resolver, sm, declarator->name, direction,
                         with_declarator_dimensions(sm, type, *declarator),
                         with_declarator_dimensions(sm, decl_type, *declarator),
                         with_declarator_dimensions(sm, signal_decl_type, *declarator));
        }
    }
}


static int find_instance_end_line(const std::vector<std::string_view>& lines, int start_line) {
    if (start_line < 0)
        start_line = 0;
    for (int line = start_line; line < (int)lines.size(); ++line) {
        if (lines[line].find(';') != std::string_view::npos)
            return line;
    }
    return lines.empty() ? 0 : (int)lines.size() - 1;
}

static int syntax_end_line0(const slang::SourceManager& sm, const SyntaxNode& node,
                            int fallback_line) {
    const auto end = node.sourceRange().end();
    if (!end.valid())
        return fallback_line;
    const auto line = sm.getLineNumber(end);
    return line > 0 ? static_cast<int>(line) - 1 : fallback_line;
}

namespace {

struct InstanceScan {
    std::vector<InstanceEntry>* out;
    SyntaxIndex* index;
    SourceFileIdResolver* resolver;
    const slang::SourceManager* sm;
    std::string_view source;
    std::vector<std::string_view> lines;
    std::string_view parent_module;

    void members(const SyntaxList<MemberSyntax>& list) {
        for (const auto* member : list) {
            if (member)
                member_node(*member);
        }
    }

    void member_node(const MemberSyntax& member) {
        if (const auto* hierarchy = member.as_if<HierarchyInstantiationSyntax>()) {
            hierarchy_node(*hierarchy);
            return;
        }
        // Instantiations also live inside generate constructs.  The parser keeps
        // those bodies as nested member lists rather than splicing them into the
        // enclosing module, so descend into every generate form to index them
        // the same way as direct module members.
        if (const auto* region = member.as_if<GenerateRegionSyntax>()) {
            members(region->members);
        } else if (const auto* block = member.as_if<GenerateBlockSyntax>()) {
            members(block->members);
        } else if (const auto* loop = member.as_if<LoopGenerateSyntax>()) {
            member_node(*loop->block);
        } else if (const auto* cond = member.as_if<IfGenerateSyntax>()) {
            member_node(*cond->block);
            if (cond->elseClause)
                clause(*cond->elseClause->clause);
        } else if (const auto* sel = member.as_if<CaseGenerateSyntax>()) {
            for (const auto* item : sel->items) {
                if (const auto* standard = item->as_if<StandardCaseItemSyntax>())
                    clause(*standard->clause);
                else if (const auto* def = item->as_if<DefaultCaseItemSyntax>())
                    clause(*def->clause);
            }
        }
    }

    // `else` arms and case-item bodies are typed as bare SyntaxNode because they
    // can also hold non-member syntax; only member-shaped clauses can contain
    // instantiations.
    void clause(const SyntaxNode& node) {
        if (const auto* member = node.as_if<MemberSyntax>())
            member_node(*member);
    }

    void hierarchy_node(const HierarchyInstantiationSyntax& hierarchy) {
        const std::string module_name = token_value_text(hierarchy.type);
        for (const auto* instance : hierarchy.instances) {
            if (!instance)
                continue;

            InstanceEntry entry;
            entry.module_name = module_name;
            entry.parent_module = std::string(parent_module);
            if (instance->decl) {
                entry.instance_name = token_value_text(instance->decl->name);
                entry.file_id = resolver->for_token(*index, *sm, instance->decl->name);
                entry.line = token_pos_line1_col0(*sm, instance->decl->name).first;
            }
            entry.start_line = entry.line > 0 ? entry.line - 1 : 0;
            // Use the parsed hierarchy range when available.  A raw ';' search
            // is a best-effort fallback only: comments and string literals can
            // legally contain semicolons before the actual instance terminator.
            const int fallback_end_line = source.empty()
                                              ? entry.start_line
                                              : find_instance_end_line(lines, entry.start_line);
            entry.end_line = syntax_end_line0(*sm, hierarchy, fallback_end_line);

            for (const auto* connection : instance->connections) {
                if (!connection)
                    continue;
                const auto* named = connection->as_if<NamedPortConnectionSyntax>();
                if (!named)
                    continue;

                auto [line, col] = token_pos_line1_col0(*sm, named->name);
                auto [paren_line, paren_col] = token_pos_line1_col0(*sm, named->openParen);
                entry.connections.push_back(NamedPortConn{
                    .port_name = token_value_text(named->name),
                    .signal_name = simple_identifier_from_expr(named->expr),
                    .file_id = resolver->for_token(*index, *sm, named->name),
                    .line = line,
                    .col = col,
                    .hint_col = paren_line == line ? paren_col + 1 : col,
                });
            }
            out->push_back(std::move(entry));
        }
    }
};

} // namespace

static void extract_instances(const SyntaxList<MemberSyntax>& members,
                              std::vector<InstanceEntry>& out, SyntaxIndex& index,
                              SourceFileIdResolver& resolver, const slang::SourceManager& sm,
                              std::string_view source, std::string_view parent_module) {
    InstanceScan scan{
        .out = &out,
        .index = &index,
        .resolver = &resolver,
        .sm = &sm,
        .source = source,
        // Split lines once here so find_instance_end_line doesn't re-split per instance.
        .lines = source.empty() ? std::vector<std::string_view>{} : split_lines_view(source),
        .parent_module = parent_module,
    };
    scan.members(members);
}

static void process_class(const ClassDeclarationSyntax& cls, SyntaxIndex& index,
                          SourceFileIdResolver& resolver, const slang::SourceManager& sm,
                          std::string parent_scope);

static void process_typedef(const TypedefDeclarationSyntax& td, SyntaxIndex& index,
                            SourceFileIdResolver& resolver, const slang::SourceManager& sm,
                            std::string parent_scope);

static void process_module(const ModuleDeclarationSyntax& module, SyntaxIndex& index,
                           SourceFileIdResolver& resolver, const slang::SourceManager& sm,
                           std::string_view source, IndexDepth depth = IndexDepth::Full) {
    ModuleEntry entry;
    entry.name = token_value_text(module.header->name);
    entry.file_id = resolver.for_token(index, sm, module.header->name);
    auto [line, col] = token_pos_line1_col0(sm, module.header->name);
    entry.line = line;
    entry.col = col;

    // Extract parameter ports from #( ... )
    if (module.header->parameters) {
        for (const auto* param_base : module.header->parameters->declarations) {
            if (!param_base)
                continue;
            const auto* param = param_base->as_if<ParameterDeclarationSyntax>();
            if (!param)
                continue;
            const std::string direction = token_value_text(param->keyword); // "parameter" or "localparam"
            const std::string type_text = render_syntax_node_text(sm, *param->type);
            for (const auto* decl : param->declarators) {
                if (!decl)
                    continue;
                std::string default_val;
                if (decl->initializer)
                    default_val = render_syntax_node_text(sm, *decl->initializer->expr);
                add_port(entry.ports, index, resolver, sm, decl->name, direction, type_text, type_text, {},
                         std::move(default_val));
            }
        }
    }

    if (module.header->ports) {
        if (const auto* ansi = module.header->ports->as_if<AnsiPortListSyntax>())
            extract_ansi_ports(*ansi, entry.ports, index, resolver, sm);
    }
    extract_port_declarations(module.members, entry.ports, index, resolver, sm);
    extract_instances(module.members, index.instances, index, resolver, sm, source, entry.name);
    for (const auto* member : module.members) {
        if (!member)
            continue;
        if (const auto* modport = member->as_if<ModportDeclarationSyntax>()) {
            for (const auto* item : modport->items) {
                if (!item)
                    continue;
                auto [ml, mc] = token_pos_line1_col0(sm, item->name);
                entry.modports.push_back(ModportEntry{
                    .name = token_value_text(item->name),
                    .file_id = resolver.for_token(index, sm, item->name),
                    .line = ml,
                    .col = mc,
                });
            }
        }
    }
    for (size_t i = 0; i < entry.ports.size(); ++i)
        entry.port_by_name.try_emplace(entry.ports[i].name, i);

    for (const auto& p : entry.ports) {
        const bool is_parameter = p.direction == "parameter" || p.direction == "localparam";
        index.values.push_back(ValueEntry{
            .name = p.name,
            .type = p.type,
            .kind = is_parameter ? p.direction : std::string("port"),
            .parent_scope = entry.name,
            .default_value = is_parameter ? p.default_value : std::string{},
            .file_id = p.file_id,
            .line = p.line,
            .col = p.col,
        });
    }

    // Module-level data and function members: collected for all depths.
    // These are member-accessible (e.g. vif.valid) and cheap to index — one
    // pass over top-level members only, no tree walk.  LocalVariableVisitor
    // below (block-local vars inside always/initial/function bodies) is the
    // expensive full-tree walk and is skipped for Declarations depth.
    // Packages reach this loop through process_package(), which registers the
    // package name first.  Members of a package additionally get a scoped-name
    // entry so `pkg::member` resolves in O(1) without a linear value scan.
    const bool in_package = index.package_names.count(entry.name) > 0;
    auto note_package_member = [&](std::string_view member_name) {
        if (in_package)
            index.package_value_by_scoped_name.try_emplace(
                package_scoped_key(entry.name, member_name), index.values.size());
    };

    for (const auto* member : module.members) {
        if (!member)
            continue;
        if (const auto* data = member->as_if<DataDeclarationSyntax>()) {
            std::optional<std::string> type_text;
            for (const auto* decl : data->declarators) {
                if (!decl)
                    continue;
                if (!resolver.wants_declaration(index, sm, decl->name))
                    continue;
                if (!type_text)
                    type_text = render_syntax_node_text(sm, *data->type);
                auto [vl, vc] = token_pos_line1_col0(sm, decl->name);
                note_package_member(token_value_text(decl->name));
                index.values.push_back(ValueEntry{
                    .name = token_value_text(decl->name),
                    .type = with_declarator_dimensions(sm, *type_text, *decl),
                    .kind = "variable",
                    .parent_scope = entry.name,
                    .file_id = resolver.for_token(index, sm, decl->name),
                    .line = vl,
                    .col = vc,
                });
            }
        } else if (const auto* fn = member->as_if<FunctionDeclarationSyntax>()) {
            const auto& proto = *fn->prototype;
            const auto* id_name = proto.name->as_if<IdentifierNameSyntax>();
            const auto name_tok = id_name ? id_name->identifier : proto.keyword;
            const auto fn_name = id_name ? std::string(id_name->identifier.valueText())
                                         : render_syntax_node_text(sm, *proto.name);
            auto [nl, nc] = token_pos_line1_col0(sm, name_tok);
            note_package_member(fn_name);
            index.values.push_back(ValueEntry{
                .name = fn_name,
                .type = render_syntax_node_text(sm, *proto.returnType),
                .kind = std::string(proto.keyword.valueText()),
                .parent_scope = entry.name,
                .file_id = resolver.for_token(index, sm, name_tok),
                .line = nl,
                .col = nc,
                .signature = make_fn_signature(proto, fn_name, sm),
            });
        } else if (const auto* ps = member->as_if<ParameterDeclarationStatementSyntax>()) {
            // Body parameters (`localparam DEPTH = 1;`) as opposed to header
            // `#(...)` parameters.  Without these a closed file's body
            // parameters are absent from the shard entirely, so neither
            // go-to-definition nor hover can see them.  This is also the sole
            // owner of package parameter values: process_package() delegates
            // here rather than pushing its own duplicate ValueEntry.
            if (const auto* param = ps->parameter->as_if<ParameterDeclarationSyntax>()) {
                // Rendered on the first declarator that is actually kept: a
                // shared header's parameter bulk would otherwise pay type and
                // initializer rendering once per including file.
                std::optional<std::string> type_text;
                std::string kind;
                for (const auto* decl : param->declarators) {
                    if (!decl)
                        continue;
                    if (!resolver.wants_declaration(index, sm, decl->name))
                        continue;
                    if (!type_text) {
                        type_text = render_syntax_node_text(sm, *param->type);
                        kind = token_value_text(param->keyword);
                    }
                    auto [vl, vc] = token_pos_line1_col0(sm, decl->name);
                    note_package_member(token_value_text(decl->name));
                    index.values.push_back(ValueEntry{
                        .name = token_value_text(decl->name),
                        .type = *type_text,
                        .kind = kind,
                        .parent_scope = entry.name,
                        .default_value = decl->initializer
                                             ? render_syntax_node_text(sm, *decl->initializer->expr)
                                             : std::string{},
                        .file_id = resolver.for_token(index, sm, decl->name),
                        .line = vl,
                        .col = vc,
                    });
                }
            }
        } else if (const auto* cls = member->as_if<ClassDeclarationSyntax>()) {
            // Closed/project files are represented by compact SyntaxIndex
            // shards, so module-local types must be present in the shard too.
            // This mirrors the live dynamic index and lets member access in
            // another file resolve `top::Packet` / `Packet` fields and methods
            // without retaining the closed file AST.
            process_class(*cls, index, resolver, sm, entry.name);
        } else if (const auto* td = member->as_if<TypedefDeclarationSyntax>()) {
            // Store module-scoped typedefs, including struct/union fields, so
            // `obj.field` can resolve through find_typedef_field_definition().
            // The generic AST definition visitor intentionally skips aggregate
            // field declarators; member access is the correct path for fields.
            process_typedef(*td, index, resolver, sm, entry.name);
        }
    }

    struct LocalVariableVisitor : public SyntaxVisitor<LocalVariableVisitor> {
        SyntaxIndex& index;
        SourceFileIdResolver& resolver;
        const slang::SourceManager& sm;
        const std::string& parent_scope;
        std::vector<std::pair<int, int>> scope_stack;

        LocalVariableVisitor(SyntaxIndex& index, SourceFileIdResolver& resolver,
                             const slang::SourceManager& sm, const std::string& parent_scope,
                             std::pair<int, int> module_range)
            : index(index), resolver(resolver), sm(sm), parent_scope(parent_scope) {
            scope_stack.push_back(module_range);
        }

        void handle(const ClassDeclarationSyntax& /*node*/) {
            // A class nested in a module owns its own member/local namespace.
            // Do not leak method-local variables from that class into the
            // enclosing RTL module's identifier completions.
        }

        void handle(const BlockStatementSyntax& node) {
            scope_stack.push_back(source_range_lines(sm, node.sourceRange()));
            visitDefault(node);
            scope_stack.pop_back();
        }

        void handle(const LocalVariableDeclarationSyntax& node) {
            const auto type_text = render_syntax_node_text(sm, *node.type);
            const auto [scope_start, scope_end] =
                scope_stack.empty() ? std::pair<int, int>{0, 0} : scope_stack.back();
            for (const auto* decl : node.declarators) {
                if (!decl)
                    continue;
                auto [vl, vc] = token_pos_line1_col0(sm, decl->name);
                index.values.push_back(ValueEntry{
                    .name = token_value_text(decl->name),
                    .type = with_declarator_dimensions(sm, type_text, *decl),
                    .kind = "variable",
                    .parent_scope = parent_scope,
                    .file_id = resolver.for_token(index, sm, decl->name),
                    .scope_start_line = scope_start,
                    .scope_end_line = scope_end,
                    .line = vl,
                    .col = vc,
                });
            }
            visitDefault(node);
        }

        void handle(const DataDeclarationSyntax& node) {
            if (scope_stack.size() <= 1) {
                visitDefault(node);
                return;
            }

            const auto type_text = render_syntax_node_text(sm, *node.type);
            const auto [scope_start, scope_end] = scope_stack.back();
            for (const auto* decl : node.declarators) {
                if (!decl)
                    continue;
                auto [vl, vc] = token_pos_line1_col0(sm, decl->name);
                index.values.push_back(ValueEntry{
                    .name = token_value_text(decl->name),
                    .type = with_declarator_dimensions(sm, type_text, *decl),
                    .kind = "variable",
                    .parent_scope = parent_scope,
                    .file_id = resolver.for_token(index, sm, decl->name),
                    .scope_start_line = scope_start,
                    .scope_end_line = scope_end,
                    .line = vl,
                    .col = vc,
                });
            }
            visitDefault(node);
        }
    };

    if (depth == IndexDepth::Full) {
        LocalVariableVisitor locals(index, resolver, sm, entry.name,
                                    source_range_lines(sm, module.sourceRange()));
        module.visit(locals);
    }

    index.module_by_name.try_emplace(entry.name, index.modules.size());
    index.modules.push_back(std::move(entry));
}

// ── New extraction functions ──────────────────────────────────────────────────

static void process_class(const ClassDeclarationSyntax& cls, SyntaxIndex& index,
                           SourceFileIdResolver& resolver, const slang::SourceManager& sm,
                           std::string parent_scope = {}) {
    ClassEntry entry;
    entry.name = token_value_text(cls.name);
    entry.file_id = resolver.for_token(index, sm, cls.name);
    entry.parent_scope = std::move(parent_scope);
    auto [line, col] = token_pos_line1_col0(sm, cls.name);
    entry.line = line;
    entry.col = col;

    if (cls.extendsClause)
        entry.base_class = render_syntax_node_text(sm, *cls.extendsClause->baseName);

    for (const auto* item : cls.items) {
        if (!item)
            continue;
        if (const auto* prop = item->as_if<ClassPropertyDeclarationSyntax>()) {
            if (const auto* data = prop->declaration->as_if<DataDeclarationSyntax>()) {
                const std::string type_text = render_syntax_node_text(sm, *data->type);
                for (const auto* decl : data->declarators) {
                    if (!decl)
                        continue;
                    auto [fl, fc] = token_pos_line1_col0(sm, decl->name);
                    entry.fields.push_back(
                        FieldEntry{.name = token_value_text(decl->name),
                                   .type = type_text,
                                   .file_id = resolver.for_token(index, sm, decl->name),
                                   .line = fl,
                                   .col = fc});
                }
            }
        } else if (const auto* meth = item->as_if<ClassMethodDeclarationSyntax>()) {
            const auto& proto = *meth->declaration->prototype;
            MethodEntry m;
            m.name = render_syntax_node_text(sm, *proto.name);
            m.return_type = render_syntax_node_text(sm, *proto.returnType);
            m.is_task = (meth->declaration->kind == SyntaxKind::TaskDeclaration);
            m.file_id = resolver.for_token(index, sm, proto.keyword);
            auto [ml, mc] = token_pos_line1_col0(sm, proto.keyword);
            m.line = ml;
            m.col = mc;
            entry.methods.push_back(std::move(m));
        }
    }

    if (!entry.parent_scope.empty() && index.package_names.count(entry.parent_scope))
        index.package_class_by_scoped_name.try_emplace(
            package_scoped_key(entry.parent_scope, entry.name), index.classes.size());
    index.class_by_name.try_emplace(entry.name, index.classes.size());
    index.classes.push_back(std::move(entry));
}

static void process_typedef(const TypedefDeclarationSyntax& td, SyntaxIndex& index,
                             SourceFileIdResolver& resolver, const slang::SourceManager& sm,
                             std::string parent_scope = {}) {
    TypedefEntry entry;
    entry.name = token_value_text(td.name);
    entry.parent_scope = std::move(parent_scope);
    entry.file_id = resolver.for_token(index, sm, td.name);
    auto [td_line, td_col] = token_pos_line1_col0(sm, td.name);
    entry.line = td_line;
    entry.col = td_col;

    if (const auto* enum_type = td.type->as_if<EnumTypeSyntax>()) {
        entry.is_enum = true;
        for (const auto* member : enum_type->members) {
            if (member) {
                auto [em_line, em_col] = token_pos_line1_col0(sm, member->name);
                entry.enum_members.push_back(EnumMemberEntry{
                    .name = token_value_text(member->name),
                    .file_id = resolver.for_token(index, sm, member->name),
                    .line = em_line,
                    .col = em_col,
                });
            }
        }
    } else if (const auto* struct_type = td.type->as_if<StructUnionTypeSyntax>()) {
        entry.is_struct = true;
        for (const auto* member : struct_type->members) {
            if (!member)
                continue;
            const std::string type_text = render_syntax_node_text(sm, *member->type);
            for (const auto* decl : member->declarators) {
                if (!decl)
                    continue;
                auto [fl, fc] = token_pos_line1_col0(sm, decl->name);
                entry.fields.push_back(FieldEntry{
                    .name = token_value_text(decl->name),
                    .type = with_declarator_dimensions(sm, type_text, *decl),
                    .file_id = resolver.for_token(index, sm, decl->name),
                    .line = fl,
                    .col = fc,
                });
            }
        }
    } else {
        entry.resolved = render_syntax_node_text(sm, *td.type);
    }

    if (!entry.parent_scope.empty() && index.package_names.count(entry.parent_scope))
        index.package_type_by_scoped_name.try_emplace(
            package_scoped_key(entry.parent_scope, entry.name), index.typedefs.size());
    index.typedef_by_name.try_emplace(entry.name, index.typedefs.size());
    index.typedefs.push_back(std::move(entry));
}

static void process_package(const ModuleDeclarationSyntax& pkg, SyntaxIndex& index,
                             SourceFileIdResolver& resolver, const slang::SourceManager& sm,
                             std::string_view source, IndexDepth depth = IndexDepth::Full) {
    const std::string pkg_name = token_value_text(pkg.header->name);
    // Register the package name before walking members: process_module() indexes
    // package parameters, typedefs, and classes, and the package-scoped lookup
    // maps are only filled for parent scopes already known to be packages.
    index.package_names.insert(pkg_name);
    process_module(pkg, index, resolver, sm, source, depth);

    // Collect exported symbol names only.
    //
    // process_module() above already walked these same members and owns every
    // resulting entry: typedefs and classes through process_typedef() /
    // process_class(), and functions, variables, and parameters as ValueEntry.
    // Re-processing them here would duplicate each one in index.typedefs /
    // index.classes / index.values, which in turn duplicates the synthetic
    // reference occurrences built from index.values.
    std::vector<std::string> symbols;
    for (const auto* member : pkg.members) {
        if (!member)
            continue;
        if (const auto* td = member->as_if<TypedefDeclarationSyntax>()) {
            symbols.push_back(token_value_text(td->name));
            // Enum members are exported alongside the typedef that declares
            // them, but they are not separate members of the package body, so
            // process_module() never sees them.
            if (const auto* enum_type = td->type->as_if<EnumTypeSyntax>()) {
                for (const auto* enum_member : enum_type->members) {
                    if (enum_member)
                        symbols.push_back(token_value_text(enum_member->name));
                }
            }
        } else if (const auto* cls = member->as_if<ClassDeclarationSyntax>()) {
            symbols.push_back(token_value_text(cls->name));
        } else if (const auto* fn = member->as_if<FunctionDeclarationSyntax>()) {
            const auto& proto = *fn->prototype;
            const auto* id_name = proto.name->as_if<IdentifierNameSyntax>();
            symbols.push_back(id_name ? std::string(id_name->identifier.valueText())
                                      : render_syntax_node_text(sm, *proto.name));
        } else if (const auto* data = member->as_if<DataDeclarationSyntax>()) {
            for (const auto* decl : data->declarators) {
                if (decl)
                    symbols.push_back(token_value_text(decl->name));
            }
        } else if (const auto* ps = member->as_if<ParameterDeclarationStatementSyntax>()) {
            if (const auto* param = ps->parameter->as_if<ParameterDeclarationSyntax>()) {
                for (const auto* decl : param->declarators) {
                    if (decl)
                        symbols.push_back(token_value_text(decl->name));
                }
            }
        }
    }
    index.package_symbols[pkg_name] = std::move(symbols);
}

static void process_member(const MemberSyntax& member, SyntaxIndex& index,
                            SourceFileIdResolver& resolver, const slang::SourceManager& sm,
                            std::string_view source, IndexDepth depth = IndexDepth::Full) {
    if (const auto* mod = member.as_if<ModuleDeclarationSyntax>()) {
        if (member.kind == SyntaxKind::InterfaceDeclaration) {
            process_module(*mod, index, resolver, sm, source, depth);
            index.interface_names.insert(token_value_text(mod->header->name));
        } else if (member.kind == SyntaxKind::PackageDeclaration) {
            process_package(*mod, index, resolver, sm, source, depth);
        } else {
            process_module(*mod, index, resolver, sm, source, depth);
        }
    } else if (const auto* cls = member.as_if<ClassDeclarationSyntax>()) {
        process_class(*cls, index, resolver, sm);
    } else if (const auto* td = member.as_if<TypedefDeclarationSyntax>()) {
        process_typedef(*td, index, resolver, sm);
    }
}

static void collect_imports(const SyntaxNode& root, SyntaxIndex& index,
                            SourceFileIdResolver& resolver, const slang::SourceManager& sm) {
    struct ScopeFrame {
        std::string name;
        int end_line{0};
    };

    struct ImportVisitor : public SyntaxVisitor<ImportVisitor> {
        SyntaxIndex& index;
        SourceFileIdResolver& resolver;
        const slang::SourceManager& sm;
        std::vector<ScopeFrame> scope_stack;

        ImportVisitor(SyntaxIndex& index, SourceFileIdResolver& resolver,
                      const slang::SourceManager& sm) :
            index(index), resolver(resolver), sm(sm) {}

        std::string current_scope() const {
            return scope_stack.empty() ? std::string{} : scope_stack.back().name;
        }

        int current_end_line() const {
            return scope_stack.empty() ? 0 : scope_stack.back().end_line;
        }

        void push_scope(std::string name, slang::SourceRange range) {
            auto [start, end] = source_range_lines(sm, range);
            (void)start;
            scope_stack.push_back(ScopeFrame{.name = std::move(name), .end_line = end});
        }

        void handle(const ModuleDeclarationSyntax& node) {
            push_scope(token_value_text(node.header->name), node.sourceRange());
            visitDefault(node);
            scope_stack.pop_back();
        }

        void handle(const ClassDeclarationSyntax& node) {
            push_scope(token_value_text(node.name), node.sourceRange());
            visitDefault(node);
            scope_stack.pop_back();
        }

        void handle(const PackageImportDeclarationSyntax& node) {
            const auto [decl_line, decl_col] = token_pos_line1_col0(sm, node.keyword);
            (void)decl_col;

            for (const auto* item : node.items) {
                if (!item)
                    continue;

                ImportEntry entry;
                entry.package_name = token_value_text(item->package);
                entry.wildcard = item->item.kind == slang::parsing::TokenKind::Star;
                if (!entry.wildcard)
                    entry.symbol_name = token_value_text(item->item);
                entry.parent_scope = current_scope();
                entry.file_id = resolver.for_token(index, sm, item->package);
                entry.start_line = decl_line;
                entry.end_line = current_end_line();

                if (!entry.package_name.empty())
                    index.imports.push_back(std::move(entry));
            }

            visitDefault(node);
        }
    };

    ImportVisitor visitor(index, resolver, sm);
    root.visit(visitor);
}

// ─────────────────────────────────────────────────────────────────────────────

SyntaxIndex SyntaxIndex::build(const slang::syntax::SyntaxTree& tree, std::string_view source,
                               IndexDepth depth, std::string_view restrict_to_uri) {
    SyntaxIndex index;
    SourceFileIdResolver resolver;
    const auto& sm = tree.sourceManager();
    const auto& root = tree.root();

    // The words of the scoped file bound both what its occurrence tables can be
    // asked for and which of another file's declarations it could ever resolve
    // against.  An unrestricted build indexes every buffer the tree covers and
    // has no such bound, so it keeps everything.
    //
    // The text is looked up by path rather than taken from @p source so the
    // filter cannot be wrong about which file it is bounding.  Callers do pass
    // the scoped file's text today, but @p source is documented only as the
    // text to resolve instantiation end lines against, and a filter that
    // silently drops names when that assumption stops holding would degrade
    // resolution quietly rather than fail.  getAllBuffers() is documented as
    // not thread safe, which is why this stays behind the restricted-build
    // check — those run on background workers that each own their
    // SourceManager, while unrestricted current-file builds may share one.
    std::unordered_set<std::string_view> mentioned_names;
    bool mentions_ready = false;
    std::string_view scoped_text;
    if (!restrict_to_uri.empty()) {
        resolver.restrict_to_uri(std::string(restrict_to_uri));

        // @p source is the scoped file's own text only when this build owns the
        // tree.  A header shard is built from an *includer's* tree and handed
        // the header's text instead, and it must keep every declaration it
        // finds, because it is the shard everyone else resolves against.
        // Comparing against the buffer the tree root starts in tells the two
        // apart in O(1), without normalizing any path.
        const auto root_text = sm.getSourceText(root.sourceRange().start().buffer());
        if (!source.empty() && root_text.data() == source.data() &&
            root_text.size() == source.size())
            scoped_text = source;
    }
    auto ensure_mentions = [&]() -> const std::unordered_set<std::string_view>* {
        if (!mentions_ready) {
            mentioned_names = collect_mentioned_names(scoped_text, tree);
            mentions_ready = true;
        }
        return &mentioned_names;
    };
    if (!scoped_text.empty())
        resolver.set_mentions_provider(ensure_mentions);

    if (const auto* compilation_unit = root.as_if<CompilationUnitSyntax>()) {
        for (const auto* member : compilation_unit->members) {
            if (member)
                process_member(*member, index, resolver, sm, source, depth);
        }
    } else if (const auto* member = root.as_if<MemberSyntax>()) {
        // slang can expose a single-file design element directly as the
        // SyntaxTree root instead of wrapping it in CompilationUnitSyntax.
        //
        // Examples:
        //
        //     package p; ... endpackage
        //     interface bus_if; ... endinterface
        //     class cfg; ... endclass
        //     typedef enum { IDLE, BUSY } state_t;
        //
        // The compilation-unit path above sends every top-level item through
        // process_member(), which preserves package/interface/class/typedef
        // identity.  A direct-root member must use the same dispatch path;
        // otherwise live-open standalone files reached by go-to-definition can
        // be indexed differently from their disk extra-file snapshots.
        process_member(*member, index, resolver, sm, source, depth);
    }

    if (depth == IndexDepth::Full)
        collect_imports(root, index, resolver, sm);

    // Only filter the tables when collection already found something foreign to
    // drop.  If it did not, every value in the index is this file's own and
    // there is nothing the filter could remove.
    collect_combined_occurrences(tree, root, index, sm, restrict_to_uri,
                                 mentions_ready ? &mentioned_names : nullptr);
    if (!index.source_files.empty())
        index.include_dependencies = collect_include_dependency_uris(sm, index.source_files.front());

    // Macros are queried from the live current-file layer.  Extra-file macro
    // entries are intentionally skipped by Declarations depth to avoid both
    // project-wide noise and the iteration cost on every .f background parse.
    if (depth == IndexDepth::Full) {
        // Note that slang also reports its built-in preprocessor macros here.
        // Those built-ins have no source location, so indexing them would leak
        // parser internals into completion and would make go-to-definition jump
        // to a fallback location such as line 1 of the current file.  Skip them.
        for (const auto* def : tree.getDefinedMacros()) {
            if (!def)
                continue;

            if (!index_macro_has_user_source_location(sm, def->name))
                continue;

            MacroEntry mac = macro_entry_from_define(index, resolver, sm, *def);
            if (mac.name.empty())
                continue;

            index.macros.push_back(std::move(mac));
        }
    }

    return index;
}

SourceFileID SyntaxIndex::intern_source_file(std::string uri) {
    if (uri.empty())
        return kInvalidSourceFileID;

    if (auto it = source_file_ids.find(uri); it != source_file_ids.end())
        return it->second;

    const auto id = static_cast<SourceFileID>(source_files.size());
    source_files.push_back(std::move(uri));
    source_file_ids.emplace(source_files.back(), id);
    return id;
}

std::string SyntaxIndex::source_uri(SourceFileID file_id) const {
    if (file_id == kInvalidSourceFileID || file_id >= source_files.size())
        return {};
    return source_files[file_id];
}

static SourceFileID remap_file_id(const std::vector<SourceFileID>& remap, SourceFileID id) {
    if (id == kInvalidSourceFileID || id >= remap.size())
        return kInvalidSourceFileID;
    return remap[id];
}

void SyntaxIndex::merge(const SyntaxIndex& other) {
    std::vector<SourceFileID> file_remap;
    file_remap.reserve(other.source_files.size());
    for (const auto& uri : other.source_files)
        file_remap.push_back(intern_source_file(uri));

    auto remap_field = [&](FieldEntry& field) { field.file_id = remap_file_id(file_remap, field.file_id); };
    auto remap_module = [&](ModuleEntry& module) {
        module.file_id = remap_file_id(file_remap, module.file_id);
        for (auto& port : module.ports)
            port.file_id = remap_file_id(file_remap, port.file_id);
        for (auto& modport : module.modports)
            modport.file_id = remap_file_id(file_remap, modport.file_id);
    };
    auto remap_class = [&](ClassEntry& cls) {
        cls.file_id = remap_file_id(file_remap, cls.file_id);
        for (auto& field : cls.fields)
            remap_field(field);
        for (auto& method : cls.methods)
            method.file_id = remap_file_id(file_remap, method.file_id);
    };
    auto remap_typedef = [&](TypedefEntry& td) {
        td.file_id = remap_file_id(file_remap, td.file_id);
        for (auto& member : td.enum_members)
            member.file_id = remap_file_id(file_remap, member.file_id);
        for (auto& field : td.fields)
            remap_field(field);
    };

    // modules (includes interfaces and packages stored as modules)
    for (const auto& m : other.modules) {
        if (!module_by_name.count(m.name)) {
            auto copy = m;
            remap_module(copy);
            module_by_name[m.name] = modules.size();
            modules.push_back(std::move(copy));
        }
    }
    for (auto instance : other.instances) {
        instance.file_id = remap_file_id(file_remap, instance.file_id);
        for (auto& conn : instance.connections)
            conn.file_id = remap_file_id(file_remap, conn.file_id);
        instances.push_back(std::move(instance));
    }
    interface_names.insert(other.interface_names.begin(), other.interface_names.end());
    package_names.insert(other.package_names.begin(), other.package_names.end());
    for (const auto& [pkg, syms] : other.package_symbols)
        package_symbols.try_emplace(pkg, syms);

    // classes.  Bare-name dedup stays first-wins, but an entry must still be
    // appended when its package-scoped key is new: two packages may declare the
    // same class name, and dropping the second would make `pkg_b::foo`
    // unresolvable.
    for (const auto& c : other.classes) {
        const bool scoped =
            !c.parent_scope.empty() && other.package_names.count(c.parent_scope) > 0;
        const auto scoped_key = scoped ? package_scoped_key(c.parent_scope, c.name) : std::string{};
        const bool scoped_is_new = scoped && !package_class_by_scoped_name.count(scoped_key);
        if (class_by_name.count(c.name) && !scoped_is_new)
            continue;
        auto copy = c;
        remap_class(copy);
        if (scoped_is_new)
            package_class_by_scoped_name.emplace(scoped_key, classes.size());
        class_by_name.try_emplace(c.name, classes.size());
        classes.push_back(std::move(copy));
    }

    // typedefs — same scoped-key rule as classes above.
    for (const auto& t : other.typedefs) {
        const bool scoped =
            !t.parent_scope.empty() && other.package_names.count(t.parent_scope) > 0;
        const auto scoped_key = scoped ? package_scoped_key(t.parent_scope, t.name) : std::string{};
        const bool scoped_is_new = scoped && !package_type_by_scoped_name.count(scoped_key);
        if (typedef_by_name.count(t.name) && !scoped_is_new)
            continue;
        auto copy = t;
        remap_typedef(copy);
        if (scoped_is_new)
            package_type_by_scoped_name.emplace(scoped_key, typedefs.size());
        typedef_by_name.try_emplace(t.name, typedefs.size());
        typedefs.push_back(std::move(copy));
    }

    for (auto macro : other.macros) {
        macro.file_id = remap_file_id(file_remap, macro.file_id);
        macros.push_back(std::move(macro));
    }
    for (auto value : other.values) {
        value.file_id = remap_file_id(file_remap, value.file_id);
        if (!value.parent_scope.empty() && other.package_names.count(value.parent_scope))
            package_value_by_scoped_name.try_emplace(
                package_scoped_key(value.parent_scope, value.name), values.size());
        values.push_back(std::move(value));
    }
    for (auto import : other.imports) {
        import.file_id = remap_file_id(file_remap, import.file_id);
        imports.push_back(std::move(import));
    }
    for (auto reference : other.references) {
        reference.file_id = remap_file_id(file_remap, reference.file_id);
        references.push_back(std::move(reference));
    }
    std::unordered_set<std::string> include_seen(include_dependencies.begin(),
                                                 include_dependencies.end());
    for (const auto& uri : other.include_dependencies) {
        if (include_seen.insert(uri).second)
            include_dependencies.push_back(uri);
    }
}
