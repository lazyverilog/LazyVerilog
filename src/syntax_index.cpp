#include "syntax_index.hpp"
#include <algorithm>
#include <cctype>
#include <optional>
#include <slang/syntax/AllSyntax.h>
#include <slang/syntax/SyntaxTree.h>
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
                     std::string decl_type, std::string signal_decl_type) {
    if (!name)
        return;

    auto [line, col] = token_pos(sm, name);
    ports.push_back(PortEntry{
        .name = tok_str(name),
        .direction = std::move(direction),
        .type = std::move(type),
        .decl_type = std::move(decl_type),
        .signal_decl_type = std::move(signal_decl_type),
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

    if (module.header->ports) {
        if (const auto* ansi = module.header->ports->as_if<AnsiPortListSyntax>())
            extract_ansi_ports(*ansi, entry.ports, sm);
    }
    extract_port_declarations(module.members, entry.ports, sm);
    extract_instances(module.members, index.instances, sm, source, entry.name);
    for (size_t i = 0; i < entry.ports.size(); ++i)
        entry.port_by_name.try_emplace(entry.ports[i].name, i);
    index.module_by_name.try_emplace(entry.name, index.modules.size());
    index.modules.push_back(std::move(entry));
}

SyntaxIndex SyntaxIndex::build(const slang::syntax::SyntaxTree& tree, std::string_view source) {
    SyntaxIndex index;
    const auto& sm = tree.sourceManager();
    const auto& root = tree.root();

    if (const auto* compilation_unit = root.as_if<CompilationUnitSyntax>()) {
        for (const auto* member : compilation_unit->members) {
            if (!member)
                continue;
            if (const auto* module = member->as_if<ModuleDeclarationSyntax>())
                process_module(*module, index, sm, source);
        }
    } else if (const auto* module = root.as_if<ModuleDeclarationSyntax>()) {
        process_module(*module, index, sm, source);
    }

    return index;
}
