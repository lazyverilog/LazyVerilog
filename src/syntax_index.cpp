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

static std::string tok_str(const slang::parsing::Token& token) {
    return std::string(token.valueText());
}

static std::string simple_identifier_from_expr(const PropertyExprSyntax* expr) {
    if (!expr)
        return {};
    if (const auto* prop = expr->as_if<SimplePropertyExprSyntax>()) {
        if (const auto* seq = prop->expr->as_if<SimpleSequenceExprSyntax>()) {
            if (const auto* ident = seq->expr->as_if<IdentifierNameSyntax>())
                return tok_str(ident->identifier);
        }
    }
    return {};
}

static std::pair<int, int> token_pos(const slang::SourceManager& sm,
                                     const slang::parsing::Token& token) {
    if (!token || !token.location().valid())
        return {0, 0};
    const auto line = sm.getLineNumber(token.location());
    const auto col = sm.getColumnNumber(token.location());
    return {line > 0 ? (int)line : 0, col > 0 ? (int)col - 1 : 0};
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

static void collect_macro_reference_occurrences(const slang::syntax::SyntaxTree& tree,
                                                SyntaxIndex& index) {
    const auto& sm = tree.sourceManager();

    auto add_macro_ref = [&](std::string name, SourceFileID file_id, int line, int col) {
        if (name.empty())
            return;
        index.references.push_back(ReferenceEntry{
            .name = name,
            .file_id = file_id,
            .symbol_id = SymbolID::from_canonical("macro::" + name),
            .symbol_debug = "macro::" + name,
            .line = line,
            .col = col,
            .end_col = col + (int)name.size(),
        });
    };

    // Declaration occurrences live outside the normal parsed syntax tree, so
    // collect them from slang's preprocessor macro table explicitly.
    for (const auto* def : tree.getDefinedMacros()) {
        if (!def || !index_macro_has_user_source_location(sm, def->name))
            continue;
        const auto [line, col] = token_pos(sm, def->name);
        add_macro_ref(std::string(def->name.valueText()), source_file_id_for_token(index, sm, def->name),
                      line, col);
    }

    struct Visitor : public SyntaxVisitor<Visitor> {
        SyntaxIndex& index;
        const slang::SourceManager& sm;
        decltype(add_macro_ref)& add_ref;
        std::unordered_set<std::string> seen_expansions;

        Visitor(SyntaxIndex& index, const slang::SourceManager& sm,
                decltype(add_macro_ref)& add_macro_ref)
            : index(index), sm(sm), add_ref(add_macro_ref) {}

        void visitToken(slang::parsing::Token token) {
            if (!token || !token.location().valid() || !sm.isMacroLoc(token.location()))
                return;

            const auto macro_name = sm.getMacroName(token.location());
            if (macro_name.empty())
                return;

            const auto range = sm.getExpansionRange(token.location());
            if (!range.start().valid())
                return;

            // One invocation can expand to many parser tokens.  Use the
            // spelling range as the de-duplication key so each `FOO occurrence
            // is reported once.
            const auto uri = uri_from_source_location(sm, range.start());
            const std::string key = uri + ":" + std::to_string(range.start().offset()) + ":" +
                                    std::to_string(range.end().offset());
            if (!seen_expansions.insert(key).second)
                return;

            const int line = (int)sm.getLineNumber(range.start());
            int col = (int)sm.getColumnNumber(range.start()) - 1;
            if (col < 0)
                col = 0;
            if (auto text = source_text_for_syntax_range(sm, range); text && text->starts_with('`'))
                ++col;

            add_ref(std::string(macro_name), source_file_id_for_location(index, sm, range.start()),
                          line, col);
        }
    };

    Visitor visitor(index, sm, add_macro_ref);
    tree.root().visit(visitor);
}

static MacroEntry macro_entry_from_define(SyntaxIndex& index, const slang::SourceManager& sm,
                                          const DefineDirectiveSyntax& def) {
    MacroEntry mac;
    mac.name = std::string(def.name.valueText());
    mac.file_id = source_file_id_for_token(index, sm, def.name);
    if (def.formalArguments) {
        mac.is_function_like = true;
        for (const auto* arg : def.formalArguments->args) {
            if (arg)
                mac.params.push_back(std::string(arg->name.valueText()));
        }
    }
    mac.line = token_pos(sm, def.name).first;
    return mac;
}

static std::string direction_of(const PortHeaderSyntax& header) {
    if (const auto* variable = header.as_if<VariablePortHeaderSyntax>())
        return tok_str(variable->direction).empty() ? "unknown" : tok_str(variable->direction);
    if (const auto* net = header.as_if<NetPortHeaderSyntax>())
        return tok_str(net->direction).empty() ? "unknown" : tok_str(net->direction);
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
        std::string type = tok_str(net->netType);
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
                     const slang::SourceManager& sm,
                     const slang::parsing::Token& name, std::string direction, std::string type,
                     std::string decl_type, std::string signal_decl_type,
                     std::string default_value = {}) {
    if (!name)
        return;

    auto [line, col] = token_pos(sm, name);
    ports.push_back(PortEntry{
        .name = tok_str(name),
        .file_id = source_file_id_for_token(index, sm, name),
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
                               SyntaxIndex& index, const slang::SourceManager& sm) {
    for (const auto* member : port_list.ports) {
        if (!member)
            continue;

        if (const auto* implicit = member->as_if<ImplicitAnsiPortSyntax>()) {
            add_port(ports, index, sm, implicit->declarator->name, direction_of(*implicit->header),
                     with_declarator_dimensions(sm, type_of(sm, *implicit->header),
                                                *implicit->declarator),
                     with_declarator_dimensions(sm, decl_type_of(sm, *implicit->header),
                                                *implicit->declarator),
                     with_declarator_dimensions(sm, signal_decl_type_of(sm, *implicit->header),
                                                *implicit->declarator));
        } else if (const auto* explicit_port = member->as_if<ExplicitAnsiPortSyntax>()) {
            auto direction = tok_str(explicit_port->direction);
            add_port(ports, index, sm, explicit_port->name,
                     direction.empty() ? std::string("unknown") : std::move(direction), {}, {}, {});
        }
    }
}

static void extract_port_declarations(const SyntaxList<MemberSyntax>& members,
                                      std::vector<PortEntry>& ports, SyntaxIndex& index,
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
                add_port(ports, index, sm, declarator->name, direction,
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

static void extract_instances(const SyntaxList<MemberSyntax>& members,
                              std::vector<InstanceEntry>& out, SyntaxIndex& index,
                              const slang::SourceManager& sm,
                              std::string_view source, std::string_view parent_module) {
    // Split lines once here so find_instance_end_line doesn't re-split per instance.
    const auto lines = source.empty() ? std::vector<std::string_view>{} : split_lines_view(source);

    for (const auto* member : members) {
        if (!member)
            continue;
        const auto* hierarchy = member->as_if<HierarchyInstantiationSyntax>();
        if (!hierarchy)
            continue;

        const std::string module_name = tok_str(hierarchy->type);
        for (const auto* instance : hierarchy->instances) {
            if (!instance)
                continue;

            InstanceEntry entry;
            entry.module_name = module_name;
            entry.parent_module = std::string(parent_module);
            if (instance->decl) {
                entry.instance_name = tok_str(instance->decl->name);
                entry.file_id = source_file_id_for_token(index, sm, instance->decl->name);
                entry.line = token_pos(sm, instance->decl->name).first;
            }
            entry.start_line = entry.line > 0 ? entry.line - 1 : 0;
            // Use the parsed hierarchy range when available.  A raw ';' search
            // is a best-effort fallback only: comments and string literals can
            // legally contain semicolons before the actual instance terminator.
            const int fallback_end_line = source.empty()
                                              ? entry.start_line
                                              : find_instance_end_line(lines, entry.start_line);
            entry.end_line = syntax_end_line0(sm, *hierarchy, fallback_end_line);

            for (const auto* connection : instance->connections) {
                if (!connection)
                    continue;
                const auto* named = connection->as_if<NamedPortConnectionSyntax>();
                if (!named)
                    continue;

                auto [line, col] = token_pos(sm, named->name);
                auto [paren_line, paren_col] = token_pos(sm, named->openParen);
                entry.connections.push_back(NamedPortConn{
                    .port_name = tok_str(named->name),
                    .signal_name = simple_identifier_from_expr(named->expr),
                    .file_id = source_file_id_for_token(index, sm, named->name),
                    .line = line,
                    .col = col,
                    .hint_col = paren_line == line ? paren_col + 1 : col,
                });
            }
            out.push_back(std::move(entry));
        }
    }
}

static void process_module(const ModuleDeclarationSyntax& module, SyntaxIndex& index,
                           const slang::SourceManager& sm, std::string_view source,
                           IndexDepth depth = IndexDepth::Full) {
    ModuleEntry entry;
    entry.name = tok_str(module.header->name);
    entry.file_id = source_file_id_for_token(index, sm, module.header->name);
    auto [line, col] = token_pos(sm, module.header->name);
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
            const std::string direction = tok_str(param->keyword); // "parameter" or "localparam"
            const std::string type_text = render_syntax_node_text(sm, *param->type);
            for (const auto* decl : param->declarators) {
                if (!decl)
                    continue;
                std::string default_val;
                if (decl->initializer)
                    default_val = render_syntax_node_text(sm, *decl->initializer->expr);
                add_port(entry.ports, index, sm, decl->name, direction, type_text, type_text, {},
                         std::move(default_val));
            }
        }
    }

    if (module.header->ports) {
        if (const auto* ansi = module.header->ports->as_if<AnsiPortListSyntax>())
            extract_ansi_ports(*ansi, entry.ports, index, sm);
    }
    extract_port_declarations(module.members, entry.ports, index, sm);
    extract_instances(module.members, index.instances, index, sm, source, entry.name);
    for (const auto* member : module.members) {
        if (!member)
            continue;
        if (const auto* modport = member->as_if<ModportDeclarationSyntax>()) {
            for (const auto* item : modport->items) {
                if (!item)
                    continue;
                auto [ml, mc] = token_pos(sm, item->name);
                entry.modports.push_back(ModportEntry{
                    .name = tok_str(item->name),
                    .file_id = source_file_id_for_token(index, sm, item->name),
                    .line = ml,
                    .col = mc,
                });
            }
        }
    }
    for (size_t i = 0; i < entry.ports.size(); ++i)
        entry.port_by_name.try_emplace(entry.ports[i].name, i);

    for (const auto& p : entry.ports) {
        index.values.push_back(ValueEntry{
            .name = p.name,
            .type = p.type,
            .kind = (p.direction == "parameter" || p.direction == "localparam")
                        ? p.direction
                        : std::string("port"),
            .parent_scope = entry.name,
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
    for (const auto* member : module.members) {
        if (!member)
            continue;
        if (const auto* data = member->as_if<DataDeclarationSyntax>()) {
            const std::string type_text = render_syntax_node_text(sm, *data->type);
            for (const auto* decl : data->declarators) {
                if (!decl)
                    continue;
                auto [vl, vc] = token_pos(sm, decl->name);
                index.values.push_back(ValueEntry{
                    .name = tok_str(decl->name),
                    .type = with_declarator_dimensions(sm, type_text, *decl),
                    .kind = "variable",
                    .parent_scope = entry.name,
                    .file_id = source_file_id_for_token(index, sm, decl->name),
                    .line = vl,
                    .col = vc,
                });
            }
        } else if (const auto* fn = member->as_if<FunctionDeclarationSyntax>()) {
            const auto& proto = *fn->prototype;
            auto [vl, vc] = token_pos(sm, proto.keyword);
            index.values.push_back(ValueEntry{
                .name = render_syntax_node_text(sm, *proto.name),
                .type = render_syntax_node_text(sm, *proto.returnType),
                .kind = "function",
                .parent_scope = entry.name,
                .file_id = source_file_id_for_token(index, sm, proto.keyword),
                .line = vl,
                .col = vc,
            });
        }
    }

    struct LocalVariableVisitor : public SyntaxVisitor<LocalVariableVisitor> {
        SyntaxIndex& index;
        const slang::SourceManager& sm;
        const std::string& parent_scope;
        std::vector<std::pair<int, int>> scope_stack;

        LocalVariableVisitor(SyntaxIndex& index, const slang::SourceManager& sm,
                             const std::string& parent_scope, std::pair<int, int> module_range)
            : index(index), sm(sm), parent_scope(parent_scope) {
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
                auto [vl, vc] = token_pos(sm, decl->name);
                index.values.push_back(ValueEntry{
                    .name = tok_str(decl->name),
                    .type = with_declarator_dimensions(sm, type_text, *decl),
                    .kind = "variable",
                    .parent_scope = parent_scope,
                    .file_id = source_file_id_for_token(index, sm, decl->name),
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
                auto [vl, vc] = token_pos(sm, decl->name);
                index.values.push_back(ValueEntry{
                    .name = tok_str(decl->name),
                    .type = with_declarator_dimensions(sm, type_text, *decl),
                    .kind = "variable",
                    .parent_scope = parent_scope,
                    .file_id = source_file_id_for_token(index, sm, decl->name),
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
        LocalVariableVisitor locals(index, sm, entry.name,
                                    source_range_lines(sm, module.sourceRange()));
        module.visit(locals);
    }

    index.module_by_name.try_emplace(entry.name, index.modules.size());
    index.modules.push_back(std::move(entry));
}

// ── New extraction functions ──────────────────────────────────────────────────

static void process_class(const ClassDeclarationSyntax& cls, SyntaxIndex& index,
                           const slang::SourceManager& sm, std::string parent_scope = {}) {
    ClassEntry entry;
    entry.name = tok_str(cls.name);
    entry.file_id = source_file_id_for_token(index, sm, cls.name);
    entry.parent_scope = std::move(parent_scope);
    auto [line, col] = token_pos(sm, cls.name);
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
                    auto [fl, fc] = token_pos(sm, decl->name);
                    entry.fields.push_back(
                        FieldEntry{.name = tok_str(decl->name),
                                   .type = type_text,
                                   .file_id = source_file_id_for_token(index, sm, decl->name),
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
            m.file_id = source_file_id_for_token(index, sm, proto.keyword);
            auto [ml, mc] = token_pos(sm, proto.keyword);
            m.line = ml;
            m.col = mc;
            entry.methods.push_back(std::move(m));
        }
    }

    index.class_by_name.try_emplace(entry.name, index.classes.size());
    index.classes.push_back(std::move(entry));
}

static void process_typedef(const TypedefDeclarationSyntax& td, SyntaxIndex& index,
                             const slang::SourceManager& sm, std::string parent_scope = {}) {
    TypedefEntry entry;
    entry.name = tok_str(td.name);
    entry.parent_scope = std::move(parent_scope);
    entry.file_id = source_file_id_for_token(index, sm, td.name);
    auto [td_line, td_col] = token_pos(sm, td.name);
    entry.line = td_line;
    entry.col = td_col;

    if (const auto* enum_type = td.type->as_if<EnumTypeSyntax>()) {
        entry.is_enum = true;
        for (const auto* member : enum_type->members) {
            if (member) {
                auto [em_line, em_col] = token_pos(sm, member->name);
                entry.enum_members.push_back(EnumMemberEntry{
                    .name = tok_str(member->name),
                    .file_id = source_file_id_for_token(index, sm, member->name),
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
                auto [fl, fc] = token_pos(sm, decl->name);
                entry.fields.push_back(FieldEntry{
                    .name = tok_str(decl->name),
                    .type = with_declarator_dimensions(sm, type_text, *decl),
                    .file_id = source_file_id_for_token(index, sm, decl->name),
                    .line = fl,
                    .col = fc,
                });
            }
        }
    } else {
        entry.resolved = render_syntax_node_text(sm, *td.type);
    }

    index.typedef_by_name.try_emplace(entry.name, index.typedefs.size());
    index.typedefs.push_back(std::move(entry));
}

static void process_package(const ModuleDeclarationSyntax& pkg, SyntaxIndex& index,
                             const slang::SourceManager& sm, std::string_view source,
                             IndexDepth depth = IndexDepth::Full) {
    const std::string pkg_name = tok_str(pkg.header->name);
    process_module(pkg, index, sm, source, depth);
    index.package_names.insert(pkg_name);

    // Collect exported symbol names and index nested declarations globally.
    std::vector<std::string> symbols;
    for (const auto* member : pkg.members) {
        if (!member)
            continue;
        if (const auto* td = member->as_if<TypedefDeclarationSyntax>()) {
            symbols.push_back(tok_str(td->name));
            process_typedef(*td, index, sm, pkg_name);
            if (const auto* enum_type = td->type->as_if<EnumTypeSyntax>()) {
                for (const auto* enum_member : enum_type->members) {
                    if (enum_member)
                        symbols.push_back(tok_str(enum_member->name));
                }
            }
        } else if (const auto* cls = member->as_if<ClassDeclarationSyntax>()) {
            symbols.push_back(tok_str(cls->name));
            process_class(*cls, index, sm, pkg_name);
        } else if (const auto* fn = member->as_if<FunctionDeclarationSyntax>()) {
            symbols.push_back(render_syntax_node_text(sm, *fn->prototype->name));
        } else if (const auto* data = member->as_if<DataDeclarationSyntax>()) {
            const std::string type_text = render_syntax_node_text(sm, *data->type);
            for (const auto* decl : data->declarators) {
                if (decl) {
                    symbols.push_back(tok_str(decl->name));
                    auto [vl, vc] = token_pos(sm, decl->name);
                    index.values.push_back(ValueEntry{
                        .name = tok_str(decl->name),
                        .type = with_declarator_dimensions(sm, type_text, *decl),
                        .kind = "variable",
                        .parent_scope = pkg_name,
                        .file_id = source_file_id_for_token(index, sm, decl->name),
                        .line = vl,
                        .col = vc,
                    });
                }
            }
        } else if (const auto* ps = member->as_if<ParameterDeclarationStatementSyntax>()) {
            if (const auto* param = ps->parameter->as_if<ParameterDeclarationSyntax>()) {
                const std::string type_text = render_syntax_node_text(sm, *param->type);
                for (const auto* decl : param->declarators) {
                    if (decl) {
                        symbols.push_back(tok_str(decl->name));
                        auto [vl, vc] = token_pos(sm, decl->name);
                        index.values.push_back(ValueEntry{
                            .name = tok_str(decl->name),
                            .type = type_text,
                            .kind = tok_str(param->keyword),
                            .parent_scope = pkg_name,
                            .file_id = source_file_id_for_token(index, sm, decl->name),
                            .line = vl,
                            .col = vc,
                        });
                    }
                }
            }
        }
    }
    index.package_symbols[pkg_name] = std::move(symbols);
}

static void process_member(const MemberSyntax& member, SyntaxIndex& index,
                            const slang::SourceManager& sm, std::string_view source,
                            IndexDepth depth = IndexDepth::Full) {
    if (const auto* mod = member.as_if<ModuleDeclarationSyntax>()) {
        if (member.kind == SyntaxKind::InterfaceDeclaration) {
            process_module(*mod, index, sm, source, depth);
            index.interface_names.insert(tok_str(mod->header->name));
        } else if (member.kind == SyntaxKind::PackageDeclaration) {
            process_package(*mod, index, sm, source, depth);
        } else {
            process_module(*mod, index, sm, source, depth);
        }
    } else if (const auto* cls = member.as_if<ClassDeclarationSyntax>()) {
        process_class(*cls, index, sm);
    } else if (const auto* td = member.as_if<TypedefDeclarationSyntax>()) {
        process_typedef(*td, index, sm);
    }
}

static void collect_imports(const SyntaxNode& root, SyntaxIndex& index,
                            const slang::SourceManager& sm) {
    struct ScopeFrame {
        std::string name;
        int end_line{0};
    };

    struct ImportVisitor : public SyntaxVisitor<ImportVisitor> {
        SyntaxIndex& index;
        const slang::SourceManager& sm;
        std::vector<ScopeFrame> scope_stack;

        ImportVisitor(SyntaxIndex& index, const slang::SourceManager& sm) :
            index(index), sm(sm) {}

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
            push_scope(tok_str(node.header->name), node.sourceRange());
            visitDefault(node);
            scope_stack.pop_back();
        }

        void handle(const ClassDeclarationSyntax& node) {
            push_scope(tok_str(node.name), node.sourceRange());
            visitDefault(node);
            scope_stack.pop_back();
        }

        void handle(const PackageImportDeclarationSyntax& node) {
            const auto [decl_line, decl_col] = token_pos(sm, node.keyword);
            (void)decl_col;

            for (const auto* item : node.items) {
                if (!item)
                    continue;

                ImportEntry entry;
                entry.package_name = tok_str(item->package);
                entry.wildcard = item->item.kind == slang::parsing::TokenKind::Star;
                if (!entry.wildcard)
                    entry.symbol_name = tok_str(item->item);
                entry.parent_scope = current_scope();
                entry.file_id = source_file_id_for_token(index, sm, item->package);
                entry.start_line = decl_line;
                entry.end_line = current_end_line();

                if (!entry.package_name.empty())
                    index.imports.push_back(std::move(entry));
            }

            visitDefault(node);
        }
    };

    ImportVisitor visitor(index, sm);
    root.visit(visitor);
}

// ─────────────────────────────────────────────────────────────────────────────

SyntaxIndex SyntaxIndex::build(const slang::syntax::SyntaxTree& tree, std::string_view source,
                               IndexDepth depth) {
    SyntaxIndex index;
    const auto& sm = tree.sourceManager();
    const auto& root = tree.root();

    if (const auto* compilation_unit = root.as_if<CompilationUnitSyntax>()) {
        for (const auto* member : compilation_unit->members) {
            if (member)
                process_member(*member, index, sm, source, depth);
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
        process_member(*member, index, sm, source, depth);
    }

    if (depth == IndexDepth::Full)
        collect_imports(root, index, sm);

    collect_reference_occurrences(root, index, sm);
    collect_macro_reference_occurrences(tree, index);
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

            MacroEntry mac = macro_entry_from_define(index, sm, *def);
            if (mac.name.empty())
                continue;

            index.macros.push_back(std::move(mac));
        }
    }

    index.rebuild_reference_location_lookup();
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

void SyntaxIndex::rebuild_reference_location_lookup() {
    references_by_location.clear();
    references_by_location.reserve(references.size());
    for (size_t i = 0; i < references.size(); ++i) {
        const auto& ref = references[i];
        references_by_location[ReferenceLocationKey{
            .file_id = ref.file_id,
            .line = ref.line,
            .col = ref.col,
        }].push_back(i);
    }
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

    // classes
    for (const auto& c : other.classes) {
        if (!class_by_name.count(c.name)) {
            auto copy = c;
            remap_class(copy);
            class_by_name[c.name] = classes.size();
            classes.push_back(std::move(copy));
        }
    }

    // typedefs
    for (const auto& t : other.typedefs) {
        if (!typedef_by_name.count(t.name)) {
            auto copy = t;
            remap_typedef(copy);
            typedef_by_name[t.name] = typedefs.size();
            typedefs.push_back(std::move(copy));
        }
    }

    for (auto macro : other.macros) {
        macro.file_id = remap_file_id(file_remap, macro.file_id);
        macros.push_back(std::move(macro));
    }
    for (auto value : other.values) {
        value.file_id = remap_file_id(file_remap, value.file_id);
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
    rebuild_reference_location_lookup();
}
