#include "syntax_index.hpp"
#include <algorithm>
#include <cctype>
#include <optional>
#include <slang/syntax/AllSyntax.h>
#include <slang/syntax/SyntaxTree.h>
#include <slang/syntax/SyntaxVisitor.h>
#include <slang/text/SourceManager.h>
#include <string_view>

using namespace slang;
using namespace slang::syntax;

static std::string tok_str(const slang::parsing::Token& token) {
    return std::string(token.valueText());
}

static std::string trim_copy(std::string text) {
    auto first =
        std::find_if_not(text.begin(), text.end(), [](unsigned char c) { return std::isspace(c); });
    auto last = std::find_if_not(text.rbegin(), text.rend(), [](unsigned char c) {
                    return std::isspace(c);
                }).base();
    if (first >= last)
        return {};
    return std::string(first, last);
}

static bool index_fragment_edge_is_wordlike(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$' || c == '`';
}

static bool index_needs_space_between_fragments(std::string_view previous,
                                                std::string_view next) {
    if (previous.empty() || next.empty())
        return false;

    const char prev = previous.back();
    const char curr = next.front();

    // SyntaxTree::toString() is convenient, but it can render tokens after
    // preprocessing.  The syntax index feeds hover, so it should preserve the
    // user's spelling for declarations such as:
    //
    //     input logic [`WIDTH-1:0] data [DEPTH]
    //
    // To do that we concatenate raw syntax tokens, which means we must restore
    // a few human-readable separators that trivia removal would otherwise
    // collapse ("logic[3:0]" -> "logic [3:0]", "bitsigned" -> "bit signed").
    if (index_fragment_edge_is_wordlike(prev) && index_fragment_edge_is_wordlike(curr))
        return true;
    if (index_fragment_edge_is_wordlike(prev) && (curr == '[' || curr == '{'))
        return true;
    if (prev == ',')
        return true;
    return false;
}

static std::optional<std::string> source_text_for_index_range(const slang::SourceManager& sm,
                                                              slang::SourceRange range) {
    if (!range.start().valid() || !range.end().valid())
        return std::nullopt;
    if (range.start().buffer() != range.end().buffer())
        return std::nullopt;

    const auto source = sm.getSourceText(range.start().buffer());
    const size_t begin = range.start().offset();
    const size_t end = range.end().offset();
    if (begin > end || end > source.size())
        return std::nullopt;

    return std::string(source.substr(begin, end - begin));
}

static bool same_index_source_range(slang::SourceRange lhs, slang::SourceRange rhs) {
    return lhs.start() == rhs.start() && lhs.end() == rhs.end();
}

static std::string render_index_token_text(
    const slang::SourceManager& sm, const slang::parsing::Token& token,
    std::optional<slang::SourceRange>& last_macro_range) {
    if (sm.isMacroLoc(token.location())) {
        const auto expansion_range = sm.getExpansionRange(token.location());

        // One macro invocation can produce multiple parser tokens.  Emitting
        // the expansion range once preserves exactly what the user wrote while
        // avoiding duplicates:
        //
        //     [`WIDTH-1:0]   -> one source slice, not repeated raw expansion
        if (last_macro_range && same_index_source_range(*last_macro_range, expansion_range))
            return {};
        last_macro_range = expansion_range;

        if (auto text = source_text_for_index_range(sm, expansion_range))
            return *text;
    } else {
        last_macro_range.reset();
    }

    return std::string(token.rawText());
}

static std::string render_index_syntax_text(const slang::SourceManager& sm,
                                            const slang::syntax::SyntaxNode& node) {
    std::string text;
    std::optional<slang::SourceRange> last_macro_range;
    for (auto it = node.tokens_begin(); it != node.tokens_end(); ++it) {
        const auto token = *it;
        if (!token || token.isMissing())
            continue;

        const auto fragment = render_index_token_text(sm, token, last_macro_range);
        if (fragment.empty())
            continue;

        if (index_needs_space_between_fragments(text, fragment))
            text += ' ';
        text += fragment;
    }
    return trim_copy(std::move(text));
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

static std::string direction_of(const PortHeaderSyntax& header) {
    if (const auto* variable = header.as_if<VariablePortHeaderSyntax>())
        return tok_str(variable->direction).empty() ? "unknown" : tok_str(variable->direction);
    if (const auto* net = header.as_if<NetPortHeaderSyntax>())
        return tok_str(net->direction).empty() ? "unknown" : tok_str(net->direction);
    return "unknown";
}

static std::string type_of(const slang::SourceManager& sm, const PortHeaderSyntax& header) {
    if (const auto* variable = header.as_if<VariablePortHeaderSyntax>())
        return render_index_syntax_text(sm, *variable->dataType);
    if (const auto* net = header.as_if<NetPortHeaderSyntax>())
        return render_index_syntax_text(sm, *net->dataType);
    return {};
}

static std::string decl_type_of(const slang::SourceManager& sm, const PortHeaderSyntax& header) {
    if (const auto* variable = header.as_if<VariablePortHeaderSyntax>())
        return render_index_syntax_text(sm, *variable->dataType);
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
        // text: render_index_syntax_text() preserves typedef names and macro /
        // parameter based dimensions such as `WIDTH or [DEPTH-1:0].
        std::string type = tok_str(net->netType);
        const auto data_type = render_index_syntax_text(sm, *net->dataType);
        if (!data_type.empty())
            type += (type.empty() ? "" : " ") + data_type;
        return type;
    }
    return {};
}

static std::string signal_decl_type_of(const slang::SourceManager& sm, const PortHeaderSyntax& header) {
    if (const auto* variable = header.as_if<VariablePortHeaderSyntax>())
        return render_index_syntax_text(sm, *variable->dataType);
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
        const auto data_type = render_index_syntax_text(sm, *net->dataType);
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

        const auto rendered = render_index_syntax_text(sm, *dimension);
        if (rendered.empty())
            continue;

        type += (type.empty() ? "" : " ") + rendered;
    }
    return type;
}

static void add_port(std::vector<PortEntry>& ports, const slang::SourceManager& sm,
                     const slang::parsing::Token& name, std::string direction, std::string type,
                     std::string decl_type, std::string signal_decl_type,
                     std::string default_value = {}) {
    if (!name)
        return;

    auto [line, col] = token_pos(sm, name);
    ports.push_back(PortEntry{
        .name = tok_str(name),
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
                               const slang::SourceManager& sm) {
    for (const auto* member : port_list.ports) {
        if (!member)
            continue;

        if (const auto* implicit = member->as_if<ImplicitAnsiPortSyntax>()) {
            add_port(ports, sm, implicit->declarator->name, direction_of(*implicit->header),
                     with_declarator_dimensions(sm, type_of(sm, *implicit->header),
                                                *implicit->declarator),
                     with_declarator_dimensions(sm, decl_type_of(sm, *implicit->header),
                                                *implicit->declarator),
                     with_declarator_dimensions(sm, signal_decl_type_of(sm, *implicit->header),
                                                *implicit->declarator));
        } else if (const auto* explicit_port = member->as_if<ExplicitAnsiPortSyntax>()) {
            auto direction = tok_str(explicit_port->direction);
            add_port(ports, sm, explicit_port->name,
                     direction.empty() ? std::string("unknown") : std::move(direction), {}, {}, {});
        }
    }
}

static void extract_port_declarations(const SyntaxList<MemberSyntax>& members,
                                      std::vector<PortEntry>& ports,
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
                add_port(ports, sm, declarator->name, direction,
                         with_declarator_dimensions(sm, type, *declarator),
                         with_declarator_dimensions(sm, decl_type, *declarator),
                         with_declarator_dimensions(sm, signal_decl_type, *declarator));
        }
    }
}

static std::vector<std::string_view> split_lines(std::string_view source) {
    std::vector<std::string_view> lines;
    size_t start = 0;
    while (start <= source.size()) {
        size_t end = source.find('\n', start);
        if (end == std::string_view::npos) {
            lines.push_back(source.substr(start));
            break;
        }
        lines.push_back(source.substr(start, end - start));
        start = end + 1;
    }
    if (lines.empty())
        lines.push_back({});
    return lines;
}

static int find_instance_end_line(std::string_view source, int start_line) {
    auto lines = split_lines(source);
    if (start_line < 0)
        start_line = 0;
    for (int line = start_line; line < (int)lines.size(); ++line) {
        if (lines[line].find(';') != std::string_view::npos)
            return line;
    }
    return lines.empty() ? 0 : (int)lines.size() - 1;
}

static void extract_instances(const SyntaxList<MemberSyntax>& members,
                              std::vector<InstanceEntry>& out, const slang::SourceManager& sm,
                              std::string_view source, std::string_view parent_module) {
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
                entry.line = token_pos(sm, instance->decl->name).first;
            }
            entry.start_line = entry.line > 0 ? entry.line - 1 : 0;
            entry.end_line = source.empty() ? entry.start_line
                                            : find_instance_end_line(source, entry.start_line);

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
                           const slang::SourceManager& sm, std::string_view source) {
    ModuleEntry entry;
    entry.name = tok_str(module.header->name);
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
            const std::string type_text = render_index_syntax_text(sm, *param->type);
            for (const auto* decl : param->declarators) {
                if (!decl)
                    continue;
                std::string default_val;
                if (decl->initializer)
                    default_val = render_index_syntax_text(sm, *decl->initializer->expr);
                add_port(entry.ports, sm, decl->name, direction, type_text, type_text, {},
                         std::move(default_val));
            }
        }
    }

    if (module.header->ports) {
        if (const auto* ansi = module.header->ports->as_if<AnsiPortListSyntax>())
            extract_ansi_ports(*ansi, entry.ports, sm);
    }
    extract_port_declarations(module.members, entry.ports, sm);
    extract_instances(module.members, index.instances, sm, source, entry.name);
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
            .line = p.line,
            .col = p.col,
        });
    }

    for (const auto* member : module.members) {
        if (!member)
            continue;
        if (const auto* data = member->as_if<DataDeclarationSyntax>()) {
            const std::string type_text = render_index_syntax_text(sm, *data->type);
            for (const auto* decl : data->declarators) {
                if (!decl)
                    continue;
                auto [vl, vc] = token_pos(sm, decl->name);
                index.values.push_back(ValueEntry{
                    .name = tok_str(decl->name),
                    .type = with_declarator_dimensions(sm, type_text, *decl),
                    .kind = "variable",
                    .parent_scope = entry.name,
                    .line = vl,
                    .col = vc,
                });
            }
        } else if (const auto* fn = member->as_if<FunctionDeclarationSyntax>()) {
            const auto& proto = *fn->prototype;
            auto [vl, vc] = token_pos(sm, proto.keyword);
            index.values.push_back(ValueEntry{
                .name = render_index_syntax_text(sm, *proto.name),
                .type = render_index_syntax_text(sm, *proto.returnType),
                .kind = "function",
                .parent_scope = entry.name,
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
            const auto type_text = render_index_syntax_text(sm, *node.type);
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

            const auto type_text = render_index_syntax_text(sm, *node.type);
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
                    .scope_start_line = scope_start,
                    .scope_end_line = scope_end,
                    .line = vl,
                    .col = vc,
                });
            }
            visitDefault(node);
        }
    };

    LocalVariableVisitor locals(index, sm, entry.name, source_range_lines(sm, module.sourceRange()));
    module.visit(locals);

    index.module_by_name.try_emplace(entry.name, index.modules.size());
    index.modules.push_back(std::move(entry));
}

// ── New extraction functions ──────────────────────────────────────────────────

static void process_class(const ClassDeclarationSyntax& cls, SyntaxIndex& index,
                           const slang::SourceManager& sm) {
    ClassEntry entry;
    entry.name = tok_str(cls.name);
    auto [line, col] = token_pos(sm, cls.name);
    entry.line = line;
    entry.col = col;

    if (cls.extendsClause)
        entry.base_class = render_index_syntax_text(sm, *cls.extendsClause->baseName);

    for (const auto* item : cls.items) {
        if (!item)
            continue;
        if (const auto* prop = item->as_if<ClassPropertyDeclarationSyntax>()) {
            if (const auto* data = prop->declaration->as_if<DataDeclarationSyntax>()) {
                const std::string type_text = render_index_syntax_text(sm, *data->type);
                for (const auto* decl : data->declarators) {
                    if (!decl)
                        continue;
                    auto [fl, fc] = token_pos(sm, decl->name);
                    entry.fields.push_back(
                        FieldEntry{.name = tok_str(decl->name), .type = type_text, .line = fl, .col = fc});
                }
            }
        } else if (const auto* meth = item->as_if<ClassMethodDeclarationSyntax>()) {
            const auto& proto = *meth->declaration->prototype;
            MethodEntry m;
            m.name = render_index_syntax_text(sm, *proto.name);
            m.return_type = render_index_syntax_text(sm, *proto.returnType);
            m.is_task = (meth->declaration->kind == SyntaxKind::TaskDeclaration);
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
                             const slang::SourceManager& sm) {
    // Skip if already indexed (e.g., from a package member pass)
    if (index.typedef_by_name.count(tok_str(td.name)))
        return;

    TypedefEntry entry;
    entry.name = tok_str(td.name);
    entry.line = token_pos(sm, td.name).first;

    if (const auto* enum_type = td.type->as_if<EnumTypeSyntax>()) {
        entry.is_enum = true;
        for (const auto* member : enum_type->members) {
            if (member)
                entry.enum_members.push_back(EnumMemberEntry{.name = tok_str(member->name)});
        }
    } else if (const auto* struct_type = td.type->as_if<StructUnionTypeSyntax>()) {
        entry.is_struct = true;
        for (const auto* member : struct_type->members) {
            if (!member)
                continue;
            const std::string type_text = render_index_syntax_text(sm, *member->type);
            for (const auto* decl : member->declarators) {
                if (!decl)
                    continue;
                auto [fl, fc] = token_pos(sm, decl->name);
                entry.fields.push_back(FieldEntry{
                    .name = tok_str(decl->name),
                    .type = with_declarator_dimensions(sm, type_text, *decl),
                    .line = fl,
                    .col = fc,
                });
            }
        }
    } else {
        entry.resolved = render_index_syntax_text(sm, *td.type);
    }

    index.typedef_by_name.try_emplace(entry.name, index.typedefs.size());
    index.typedefs.push_back(std::move(entry));
}

static void process_package(const ModuleDeclarationSyntax& pkg, SyntaxIndex& index,
                             const slang::SourceManager& sm, std::string_view source) {
    const std::string pkg_name = tok_str(pkg.header->name);
    process_module(pkg, index, sm, source);
    index.package_names.insert(pkg_name);

    // Collect exported symbol names and index nested declarations globally.
    std::vector<std::string> symbols;
    for (const auto* member : pkg.members) {
        if (!member)
            continue;
        if (const auto* td = member->as_if<TypedefDeclarationSyntax>()) {
            symbols.push_back(tok_str(td->name));
            process_typedef(*td, index, sm);
        } else if (const auto* cls = member->as_if<ClassDeclarationSyntax>()) {
            symbols.push_back(tok_str(cls->name));
            process_class(*cls, index, sm);
        } else if (const auto* fn = member->as_if<FunctionDeclarationSyntax>()) {
            symbols.push_back(render_index_syntax_text(sm, *fn->prototype->name));
        } else if (const auto* data = member->as_if<DataDeclarationSyntax>()) {
            const std::string type_text = render_index_syntax_text(sm, *data->type);
            for (const auto* decl : data->declarators) {
                if (decl) {
                    symbols.push_back(tok_str(decl->name));
                    auto [vl, vc] = token_pos(sm, decl->name);
                    index.values.push_back(ValueEntry{
                        .name = tok_str(decl->name),
                        .type = with_declarator_dimensions(sm, type_text, *decl),
                        .kind = "variable",
                        .parent_scope = pkg_name,
                        .line = vl,
                        .col = vc,
                    });
                }
            }
        } else if (const auto* ps = member->as_if<ParameterDeclarationStatementSyntax>()) {
            if (const auto* param = ps->parameter->as_if<ParameterDeclarationSyntax>()) {
                const std::string type_text = render_index_syntax_text(sm, *param->type);
                for (const auto* decl : param->declarators) {
                    if (decl) {
                        symbols.push_back(tok_str(decl->name));
                        auto [vl, vc] = token_pos(sm, decl->name);
                        index.values.push_back(ValueEntry{
                            .name = tok_str(decl->name),
                            .type = type_text,
                            .kind = tok_str(param->keyword),
                            .parent_scope = pkg_name,
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
                            const slang::SourceManager& sm, std::string_view source) {
    if (const auto* mod = member.as_if<ModuleDeclarationSyntax>()) {
        if (member.kind == SyntaxKind::InterfaceDeclaration) {
            process_module(*mod, index, sm, source);
            index.interface_names.insert(tok_str(mod->header->name));
        } else if (member.kind == SyntaxKind::PackageDeclaration) {
            process_package(*mod, index, sm, source);
        } else {
            process_module(*mod, index, sm, source);
        }
    } else if (const auto* cls = member.as_if<ClassDeclarationSyntax>()) {
        process_class(*cls, index, sm);
    } else if (const auto* td = member.as_if<TypedefDeclarationSyntax>()) {
        process_typedef(*td, index, sm);
    }
}

// ─────────────────────────────────────────────────────────────────────────────

SyntaxIndex SyntaxIndex::build(const slang::syntax::SyntaxTree& tree, std::string_view source) {
    SyntaxIndex index;
    const auto& sm = tree.sourceManager();
    const auto& root = tree.root();

    if (const auto* compilation_unit = root.as_if<CompilationUnitSyntax>()) {
        for (const auto* member : compilation_unit->members) {
            if (member)
                process_member(*member, index, sm, source);
        }
    } else if (const auto* module = root.as_if<ModuleDeclarationSyntax>()) {
        process_module(*module, index, sm, source);
    }

    // Macros defined at the end of this file (preprocessor output).
    for (const auto* def : tree.getDefinedMacros()) {
        if (!def)
            continue;
        MacroEntry mac;
        mac.name = std::string(def->name.valueText());
        if (mac.name.empty())
            continue;
        if (def->formalArguments) {
            mac.is_function_like = true;
            for (const auto* arg : def->formalArguments->args) {
                if (arg)
                    mac.params.push_back(std::string(arg->name.valueText()));
            }
        }
        mac.line = token_pos(sm, def->name).first;
        index.macros.push_back(std::move(mac));
    }

    return index;
}

void SyntaxIndex::merge(const SyntaxIndex& other) {
    // modules (includes interfaces and packages stored as modules)
    for (const auto& m : other.modules) {
        if (!module_by_name.count(m.name)) {
            module_by_name[m.name] = modules.size();
            modules.push_back(m);
        }
    }
    instances.insert(instances.end(), other.instances.begin(), other.instances.end());
    interface_names.insert(other.interface_names.begin(), other.interface_names.end());
    package_names.insert(other.package_names.begin(), other.package_names.end());
    for (const auto& [pkg, syms] : other.package_symbols)
        package_symbols.try_emplace(pkg, syms);

    // classes
    for (const auto& c : other.classes) {
        if (!class_by_name.count(c.name)) {
            class_by_name[c.name] = classes.size();
            classes.push_back(c);
        }
    }

    // typedefs
    for (const auto& t : other.typedefs) {
        if (!typedef_by_name.count(t.name)) {
            typedef_by_name[t.name] = typedefs.size();
            typedefs.push_back(t);
        }
    }

    macros.insert(macros.end(), other.macros.begin(), other.macros.end());
    values.insert(values.end(), other.values.begin(), other.values.end());
}
