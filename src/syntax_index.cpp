#include "syntax_index.hpp"
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

static std::string path_to_uri_for_index(const std::filesystem::path& path) {
    return "file://" + std::filesystem::absolute(path).lexically_normal().string();
}

static std::string token_uri_for_index(const slang::SourceManager& sm,
                                       const slang::parsing::Token& token) {
    if (!token || !token.location().valid())
        return {};

    const auto file_name = sm.getFileName(token.location());
    if (file_name.empty())
        return {};

    std::string file(file_name);
    if (file.starts_with("file://"))
        return file;
    return path_to_uri_for_index(file);
}

static SourceFileID token_file_id_for_index(SyntaxIndex& index, const slang::SourceManager& sm,
                                            const slang::parsing::Token& token) {
    auto uri = token_uri_for_index(sm, token);
    if (uri.empty())
        return kInvalidSourceFileID;
    return index.intern_source_file(std::move(uri));
}

static std::string location_uri_for_index(const slang::SourceManager& sm,
                                          slang::SourceLocation location) {
    if (!location.valid())
        return {};

    const auto file_name = sm.getFileName(location);
    if (file_name.empty())
        return {};

    std::string file(file_name);
    if (file.starts_with("file://"))
        return file;
    return path_to_uri_for_index(file);
}

static SourceFileID location_file_id_for_index(SyntaxIndex& index, const slang::SourceManager& sm,
                                               slang::SourceLocation location) {
    auto uri = location_uri_for_index(sm, location);
    if (uri.empty())
        return kInvalidSourceFileID;
    return index.intern_source_file(std::move(uri));
}

static std::string symbol_canonical(std::string kind, std::string scope, std::string name) {
    if (scope.empty())
        return kind + "::" + name;
    return kind + "::" + scope + "::" + name;
}

static bool is_module_value_kind(std::string_view kind) {
    return kind == "variable" || kind == "net" || kind == "parameter" || kind == "localparam" ||
           kind == "port";
}

static std::string canonical_type_name_from_index_text(std::string_view type) {
    // Keep this deliberately syntactic and cheap.  The index cannot rely on a
    // full semantic type checker for closed files, so for member references we
    // recover the last identifier-ish component from declarations such as:
    //
    //   packet_t pkt;
    //   pkg::packet_t pkt;
    //   logic [7:0] not_a_struct;
    //
    // Built-in scalar types will simply fail to match any typedef field map.
    size_t end = type.size();
    while (end > 0 && !index_fragment_edge_is_wordlike(type[end - 1]))
        --end;
    size_t begin = end;
    while (begin > 0 && index_fragment_edge_is_wordlike(type[begin - 1]))
        --begin;
    return std::string(type.substr(begin, end - begin));
}

static std::string simple_identifier_from_index_expr(const ExpressionSyntax* expr) {
    if (!expr)
        return {};
    if (const auto* ident = expr->as_if<IdentifierNameSyntax>())
        return tok_str(ident->identifier);
    return {};
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
        add_macro_ref(std::string(def->name.valueText()), token_file_id_for_index(index, sm, def->name),
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
            const auto uri = location_uri_for_index(sm, range.start());
            const std::string key = uri + ":" + std::to_string(range.start().offset()) + ":" +
                                    std::to_string(range.end().offset());
            if (!seen_expansions.insert(key).second)
                return;

            const int line = (int)sm.getLineNumber(range.start());
            int col = (int)sm.getColumnNumber(range.start()) - 1;
            if (col < 0)
                col = 0;
            if (auto text = source_text_for_index_range(sm, range); text && text->starts_with('`'))
                ++col;

            add_ref(std::string(macro_name), location_file_id_for_index(index, sm, range.start()),
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
    mac.file_id = token_file_id_for_index(index, sm, def.name);
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
        .file_id = token_file_id_for_index(index, sm, name),
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
    const auto lines = source.empty() ? std::vector<std::string_view>{} : split_lines(source);

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
                entry.file_id = token_file_id_for_index(index, sm, instance->decl->name);
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
                    .file_id = token_file_id_for_index(index, sm, named->name),
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
    entry.file_id = token_file_id_for_index(index, sm, module.header->name);
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
                    .file_id = token_file_id_for_index(index, sm, item->name),
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
                    .file_id = token_file_id_for_index(index, sm, decl->name),
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
                .file_id = token_file_id_for_index(index, sm, proto.keyword),
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
                    .file_id = token_file_id_for_index(index, sm, decl->name),
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
                    .file_id = token_file_id_for_index(index, sm, decl->name),
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
    entry.file_id = token_file_id_for_index(index, sm, cls.name);
    entry.parent_scope = std::move(parent_scope);
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
                        FieldEntry{.name = tok_str(decl->name),
                                   .type = type_text,
                                   .file_id = token_file_id_for_index(index, sm, decl->name),
                                   .line = fl,
                                   .col = fc});
                }
            }
        } else if (const auto* meth = item->as_if<ClassMethodDeclarationSyntax>()) {
            const auto& proto = *meth->declaration->prototype;
            MethodEntry m;
            m.name = render_index_syntax_text(sm, *proto.name);
            m.return_type = render_index_syntax_text(sm, *proto.returnType);
            m.is_task = (meth->declaration->kind == SyntaxKind::TaskDeclaration);
            m.file_id = token_file_id_for_index(index, sm, proto.keyword);
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
    entry.file_id = token_file_id_for_index(index, sm, td.name);
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
                    .file_id = token_file_id_for_index(index, sm, member->name),
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
            const std::string type_text = render_index_syntax_text(sm, *member->type);
            for (const auto* decl : member->declarators) {
                if (!decl)
                    continue;
                auto [fl, fc] = token_pos(sm, decl->name);
                entry.fields.push_back(FieldEntry{
                    .name = tok_str(decl->name),
                    .type = with_declarator_dimensions(sm, type_text, *decl),
                    .file_id = token_file_id_for_index(index, sm, decl->name),
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
                        .file_id = token_file_id_for_index(index, sm, decl->name),
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
                            .file_id = token_file_id_for_index(index, sm, decl->name),
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
                entry.file_id = token_file_id_for_index(index, sm, item->package);
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

static void collect_reference_occurrences(const SyntaxNode& root, SyntaxIndex& index,
                                          const slang::SourceManager& sm) {
    std::unordered_set<std::string> module_values;
    std::unordered_map<std::string, std::string> module_value_types;
    std::unordered_set<std::string> package_values;
    std::unordered_set<std::string> class_fields;
    std::unordered_set<std::string> typedef_fields;
    std::unordered_map<std::string, std::string> unique_typedef_scopes;
    std::unordered_set<std::string> ambiguous_typedef_names;
    std::unordered_map<std::string, std::string> unique_enum_member_ids;
    std::unordered_set<std::string> ambiguous_enum_members;
    std::unordered_map<std::string, std::string> unique_type_ids;
    std::unordered_set<std::string> ambiguous_type_names;
    for (const auto& value : index.values) {
        if (value.parent_scope.empty())
            continue;
        if (index.package_names.contains(value.parent_scope)) {
            package_values.insert(value.parent_scope + "\n" + value.name);
            continue;
        }
        if (is_module_value_kind(value.kind)) {
            const auto key = value.parent_scope + "\n" + value.name;
            module_values.insert(key);
            // A file can contain duplicate declarations while being edited
            // (and demo/memory_top.sv intentionally has an output named `test`
            // followed by a local `fifo_entry_t test`).  For member references
            // in a module body, the later declaration is the best syntactic
            // approximation of the object type at the use site.  Overwrite
            // instead of keeping the first declaration, otherwise `test.id`
            // can be resolved through the stale output type and miss the
            // fifo_entry_t::id field occurrence.
            module_value_types[key] = canonical_type_name_from_index_text(value.type);
        }
    }
    for (const auto& cls : index.classes) {
        const std::string class_scope =
            cls.parent_scope.empty() ? cls.name : cls.parent_scope + "::" + cls.name;
        const auto class_id = symbol_canonical("class", cls.parent_scope, cls.name);
        if (!ambiguous_type_names.contains(cls.name) && !unique_type_ids.try_emplace(cls.name, class_id).second) {
            unique_type_ids.erase(cls.name);
            ambiguous_type_names.insert(cls.name);
        }
        for (const auto& field : cls.fields)
            class_fields.insert(class_scope + "\n" + field.name);
    }
    for (const auto& td : index.typedefs) {
        const auto typedef_id = symbol_canonical("typedef", td.parent_scope, td.name);
        if (!ambiguous_type_names.contains(td.name) && !unique_type_ids.try_emplace(td.name, typedef_id).second) {
            unique_type_ids.erase(td.name);
            ambiguous_type_names.insert(td.name);
        }
        const auto typedef_scope =
            td.parent_scope.empty() ? td.name : td.parent_scope + "::" + td.name;
        if (!ambiguous_typedef_names.contains(td.name) &&
            !unique_typedef_scopes.try_emplace(td.name, typedef_scope).second) {
            unique_typedef_scopes.erase(td.name);
            ambiguous_typedef_names.insert(td.name);
        }
        for (const auto& field : td.fields)
            typedef_fields.insert(typedef_scope + "\n" + field.name);
        if (td.is_enum) {
            for (const auto& member : td.enum_members) {
                const auto member_id = symbol_canonical("enum_member", typedef_scope, member.name);
                if (!ambiguous_enum_members.contains(member.name) &&
                    !unique_enum_member_ids.try_emplace(member.name, member_id).second) {
                    unique_enum_member_ids.erase(member.name);
                    ambiguous_enum_members.insert(member.name);
                }
            }
        }
    }

    auto add_reference = [&](const slang::parsing::Token& token, std::string canonical_id) {
        if (!token || token.kind != slang::parsing::TokenKind::Identifier ||
            !token.location().valid())
            return;

        const auto [line, col] = token_pos(sm, token);
        const std::string name(token.valueText());
        if (name.empty())
            return;

        index.references.push_back(ReferenceEntry{
            .name = name,
            .file_id = token_file_id_for_index(index, sm, token),
            .symbol_id = SymbolID::from_canonical(canonical_id),
            .symbol_debug = std::move(canonical_id),
            .line = line,
            .col = col,
            .end_col = col + (int)name.size(),
        });
    };

    struct ReferenceVisitor : public SyntaxVisitor<ReferenceVisitor> {
        SyntaxIndex& index;
        const slang::SourceManager& sm;
        decltype(add_reference)& add_ref;
        const std::unordered_set<std::string>& module_values;
        const std::unordered_map<std::string, std::string>& module_value_types;
        const std::unordered_set<std::string>& package_values;
        const std::unordered_set<std::string>& class_fields;
        const std::unordered_set<std::string>& typedef_fields;
        const std::unordered_map<std::string, std::string>& unique_typedef_scopes;
        const std::unordered_map<std::string, std::string>& unique_enum_member_ids;
        const std::unordered_map<std::string, std::string>& unique_type_ids;
        std::string current_module;
        std::string current_package;
        std::string current_class;

        ReferenceVisitor(SyntaxIndex& index, const slang::SourceManager& sm,
                         decltype(add_reference)& add_reference,
                         const std::unordered_set<std::string>& module_values,
                         const std::unordered_map<std::string, std::string>& module_value_types,
                         const std::unordered_set<std::string>& package_values,
                         const std::unordered_set<std::string>& class_fields,
                         const std::unordered_set<std::string>& typedef_fields,
                         const std::unordered_map<std::string, std::string>& unique_typedef_scopes,
                         const std::unordered_map<std::string, std::string>& unique_enum_member_ids,
                         const std::unordered_map<std::string, std::string>& unique_type_ids)
            : index(index), sm(sm), add_ref(add_reference), module_values(module_values),
              module_value_types(module_value_types), package_values(package_values),
              class_fields(class_fields), typedef_fields(typedef_fields),
              unique_typedef_scopes(unique_typedef_scopes),
              unique_enum_member_ids(unique_enum_member_ids),
              unique_type_ids(unique_type_ids) {}

        void handle(const slang::syntax::ModuleDeclarationSyntax& node) {
            const std::string module_name(node.header->name.valueText());
            if (!module_name.empty())
                add_ref(node.header->name, symbol_canonical("module", {}, module_name));

            auto previous_module = current_module;
            auto previous_package = current_package;
            if (node.kind == SyntaxKind::PackageDeclaration) {
                current_package = module_name;
                current_module.clear();
            } else {
                current_module = module_name;
            }
            visitDefault(node);
            current_module = std::move(previous_module);
            current_package = std::move(previous_package);
        }

        void handle(const slang::syntax::ClassDeclarationSyntax& node) {
            const std::string class_name(node.name.valueText());
            const std::string parent_scope = !current_package.empty() ? current_package : std::string{};
            if (!class_name.empty())
                add_ref(node.name, symbol_canonical("class", parent_scope, class_name));

            auto previous_class = current_class;
            current_class = parent_scope.empty() ? class_name : parent_scope + "::" + class_name;
            visitDefault(node);
            current_class = std::move(previous_class);
        }

        void handle(const slang::syntax::TypedefDeclarationSyntax& node) {
            const std::string typedef_name(node.name.valueText());
            const std::string parent_scope = !current_package.empty() ? current_package : std::string{};
            if (!typedef_name.empty())
                add_ref(node.name, symbol_canonical("typedef", parent_scope, typedef_name));
            visitDefault(node);
        }

        void handle(const slang::syntax::HierarchyInstantiationSyntax& node) {
            const std::string module_name(node.type.valueText());
            if (!module_name.empty())
                add_ref(node.type, symbol_canonical("module", {}, module_name));

            if (node.parameters) {
                for (const auto* parameter : node.parameters->parameters) {
                    const auto* named =
                        parameter ? parameter->as_if<NamedParamAssignmentSyntax>() : nullptr;
                    if (!named)
                        continue;
                    const std::string param_name(named->name.valueText());
                    if (!module_name.empty() && !param_name.empty())
                        add_ref(named->name,
                                symbol_canonical("module_param", module_name, param_name));
                }
            }

            for (const auto* instance : node.instances) {
                if (!instance)
                    continue;

                if (instance->decl) {
                    const std::string instance_name(instance->decl->name.valueText());
                    if (!current_module.empty() && !instance_name.empty())
                        add_ref(instance->decl->name,
                                symbol_canonical("instance", current_module, instance_name));
                }

                for (const auto* connection : instance->connections) {
                    const auto* named =
                        connection ? connection->as_if<NamedPortConnectionSyntax>() : nullptr;
                    if (!named)
                        continue;

                    const std::string port_name(named->name.valueText());
                    if (!module_name.empty() && !port_name.empty())
                        add_ref(named->name, symbol_canonical("module_port", module_name, port_name));
                }
            }

            visitDefault(node);
        }

        void handle(const slang::syntax::MemberAccessExpressionSyntax& node) {
            const std::string field_name(node.name.valueText());
            const std::string object_name = simple_identifier_from_index_expr(node.left);
            if (!current_module.empty() && !field_name.empty() && !object_name.empty()) {
                const auto value_it = module_value_types.find(current_module + "\n" + object_name);
                if (value_it != module_value_types.end()) {
                    std::string typedef_scope = value_it->second;
                    if (const auto scope_it = unique_typedef_scopes.find(typedef_scope);
                        scope_it != unique_typedef_scopes.end())
                        typedef_scope = scope_it->second;

                    if (typedef_fields.contains(typedef_scope + "\n" + field_name)) {
                        add_ref(node.name,
                                symbol_canonical("typedef_field", typedef_scope, field_name));
                        if (node.left)
                            node.left->visit(*this);
                        return;
                    }
                }
            }
            visitDefault(node);
        }

        std::string object_before_member_dot(const slang::parsing::Token& token) const {
            if (!token || !token.location().valid())
                return {};
            const auto source = sm.getSourceText(token.location().buffer());
            size_t i = token.location().offset();
            if (i > source.size())
                return {};
            while (i > 0 && std::isspace(static_cast<unsigned char>(source[i - 1])))
                --i;
            if (i == 0 || source[i - 1] != '.')
                return {};
            --i;
            while (i > 0 && std::isspace(static_cast<unsigned char>(source[i - 1])))
                --i;
            const size_t end = i;
            while (i > 0 && index_fragment_edge_is_wordlike(source[i - 1]))
                --i;
            if (i == end)
                return {};
            return std::string(source.substr(i, end - i));
        }

        bool try_add_typedef_field_reference(const slang::parsing::Token& token,
                                             std::string_view field_name) {
            const auto object_name = object_before_member_dot(token);
            if (current_module.empty() || object_name.empty() || field_name.empty())
                return false;
            const auto value_it = module_value_types.find(current_module + "\n" + object_name);
            if (value_it == module_value_types.end())
                return false;

            std::string typedef_scope = value_it->second;
            if (const auto scope_it = unique_typedef_scopes.find(typedef_scope);
                scope_it != unique_typedef_scopes.end())
                typedef_scope = scope_it->second;

            if (!typedef_fields.contains(typedef_scope + "\n" + std::string(field_name)))
                return false;

            add_ref(token, symbol_canonical("typedef_field", typedef_scope, std::string(field_name)));
            return true;
        }

        void visitToken(slang::parsing::Token token) {
            if (!token || token.kind != slang::parsing::TokenKind::Identifier ||
                !token.location().valid())
                return;
            const std::string name(token.valueText());
            if (name.empty())
                return;
            if (try_add_typedef_field_reference(token, name))
                return;
            if (!current_class.empty()) {
                const std::string key = current_class + "\n" + name;
                if (class_fields.contains(key)) {
                    add_ref(token, symbol_canonical("class_field", current_class, name));
                    return;
                }
            }
            if (!current_module.empty()) {
                const std::string key = current_module + "\n" + name;
                if (module_values.contains(key)) {
                    add_ref(token, symbol_canonical("module_signal", current_module, name));
                    return;
                }
            }
            if (!current_package.empty()) {
                const std::string key = current_package + "\n" + name;
                if (package_values.contains(key)) {
                    add_ref(token, symbol_canonical("package_value", current_package, name));
                    return;
                }
            }
            if (const auto type_it = unique_type_ids.find(name); type_it != unique_type_ids.end()) {
                add_ref(token, type_it->second);
                return;
            }
            if (const auto enum_it = unique_enum_member_ids.find(name);
                enum_it != unique_enum_member_ids.end()) {
                add_ref(token, enum_it->second);
                return;
            }
            add_ref(token, "name:" + name);
        }
    };

    // Declarations are already extracted into the index.  Add owner-qualified
    // declaration references from those tables so closed-file reference search
    // can match e.g. `.clk(...)` to `module memory(input clk);` without loading
    // the declaring file's AST.
    for (const auto& module : index.modules) {
        if (!module.name.empty() && module.line > 0)
            index.references.push_back(ReferenceEntry{.name = module.name,
                                                      .file_id = module.file_id,
                                                      .symbol_id = SymbolID::from_canonical(
                                                          symbol_canonical("module", {}, module.name)),
                                                      .symbol_debug = symbol_canonical("module", {}, module.name),
                                                      .line = module.line,
                                                      .col = module.col,
                                                      .end_col = module.col +
                                                                 (int)module.name.size()});
        for (const auto& port : module.ports) {
            if (!port.name.empty() && port.line > 0) {
                const bool is_parameter =
                    port.direction == "parameter" || port.direction == "localparam";
                index.references.push_back(ReferenceEntry{
                    .name = port.name,
                    .file_id = port.file_id,
                    .symbol_id = SymbolID::from_canonical(symbol_canonical(
                        is_parameter ? "module_param" : "module_port", module.name, port.name)),
                    .symbol_debug = symbol_canonical(
                        is_parameter ? "module_param" : "module_port", module.name, port.name),
                    .line = port.line,
                    .col = port.col,
                    .end_col = port.col + (int)port.name.size()});
            }
        }
    }

    for (const auto& value : index.values) {
        if (value.name.empty() || value.parent_scope.empty() || !is_module_value_kind(value.kind) ||
            value.line <= 0)
            continue;
        if (index.package_names.contains(value.parent_scope))
            continue;
        const auto canonical = symbol_canonical("module_signal", value.parent_scope, value.name);
        index.references.push_back(ReferenceEntry{.name = value.name,
                                                  .file_id = value.file_id,
                                                  .symbol_id = SymbolID::from_canonical(canonical),
                                                  .symbol_debug = canonical,
                                                  .line = value.line,
                                                  .col = value.col,
                                                  .end_col = value.col +
                                                             (int)value.name.size()});
    }
    for (const auto& value : index.values) {
        if (value.name.empty() || value.parent_scope.empty() ||
            !index.package_names.contains(value.parent_scope) || value.line <= 0)
            continue;
        const auto canonical = symbol_canonical("package_value", value.parent_scope, value.name);
        index.references.push_back(ReferenceEntry{.name = value.name,
                                                  .file_id = value.file_id,
                                                  .symbol_id = SymbolID::from_canonical(canonical),
                                                  .symbol_debug = canonical,
                                                  .line = value.line,
                                                  .col = value.col,
                                                  .end_col = value.col +
                                                             (int)value.name.size()});
    }
    for (const auto& cls : index.classes) {
        if (cls.name.empty() || cls.line <= 0)
            continue;
        const auto class_scope =
            cls.parent_scope.empty() ? cls.name : cls.parent_scope + "::" + cls.name;
        const auto class_canonical = symbol_canonical("class", cls.parent_scope, cls.name);
        index.references.push_back(ReferenceEntry{.name = cls.name,
                                                  .file_id = cls.file_id,
                                                  .symbol_id = SymbolID::from_canonical(class_canonical),
                                                  .symbol_debug = class_canonical,
                                                  .line = cls.line,
                                                  .col = cls.col,
                                                  .end_col = cls.col + (int)cls.name.size()});
        for (const auto& field : cls.fields) {
            if (field.name.empty() || field.line <= 0)
                continue;
            const auto canonical = symbol_canonical("class_field", class_scope, field.name);
            index.references.push_back(ReferenceEntry{.name = field.name,
                                                      .file_id = field.file_id,
                                                      .symbol_id = SymbolID::from_canonical(canonical),
                                                      .symbol_debug = canonical,
                                                      .line = field.line,
                                                      .col = field.col,
                                                      .end_col = field.col +
                                                                 (int)field.name.size()});
        }
    }
    for (const auto& td : index.typedefs) {
        if (td.name.empty() || td.line <= 0)
            continue;
        const auto typedef_scope =
            td.parent_scope.empty() ? td.name : td.parent_scope + "::" + td.name;
        const auto canonical = symbol_canonical("typedef", td.parent_scope, td.name);
        index.references.push_back(ReferenceEntry{.name = td.name,
                                                  .file_id = td.file_id,
                                                  .symbol_id = SymbolID::from_canonical(canonical),
                                                  .symbol_debug = canonical,
                                                  .line = td.line,
                                                  .col = td.col,
                                                  .end_col = td.col + (int)td.name.size()});
        for (const auto& field : td.fields) {
            if (field.name.empty() || field.line <= 0)
                continue;
            const auto field_canonical =
                symbol_canonical("typedef_field", typedef_scope, field.name);
            index.references.push_back(ReferenceEntry{.name = field.name,
                                                      .file_id = field.file_id,
                                                      .symbol_id =
                                                          SymbolID::from_canonical(field_canonical),
                                                      .symbol_debug = field_canonical,
                                                      .line = field.line,
                                                      .col = field.col,
                                                      .end_col = field.col +
                                                                 (int)field.name.size()});
        }
        for (const auto& member : td.enum_members) {
            if (member.name.empty() || member.line <= 0)
                continue;
            const auto member_canonical =
                symbol_canonical("enum_member", typedef_scope, member.name);
            index.references.push_back(ReferenceEntry{.name = member.name,
                                                      .file_id = member.file_id,
                                                      .symbol_id =
                                                          SymbolID::from_canonical(member_canonical),
                                                      .symbol_debug = member_canonical,
                                                      .line = member.line,
                                                      .col = member.col,
                                                      .end_col = member.col +
                                                                 (int)member.name.size()});
        }
    }

    ReferenceVisitor visitor(index, sm, add_reference, module_values, module_value_types,
                             package_values, class_fields, typedef_fields, unique_typedef_scopes,
                             unique_enum_member_ids, unique_type_ids);
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
}
