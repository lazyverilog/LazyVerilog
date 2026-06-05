#include "syntax_index_shared.hpp"

#include <algorithm>
#include <cctype>
#include <slang/syntax/AllSyntax.h>
#include <slang/syntax/SyntaxVisitor.h>
#include <slang/text/SourceManager.h>
#include <unordered_map>
#include <unordered_set>

using namespace slang;
using namespace slang::syntax;

std::string uri_from_file_name(std::string_view file_name) {
    if (file_name.empty())
        return {};
    std::string file(file_name);
    if (file.starts_with("file://"))
        return file;
    return uri_from_path(file);
}

std::string uri_from_source_location(const slang::SourceManager& sm,
                                     slang::SourceLocation location) {
    if (!location.valid())
        return {};
    const auto file_name = sm.getFileName(location);
    return uri_from_file_name(file_name);
}

SourceFileID source_file_id_for_token(SyntaxIndex& index, const slang::SourceManager& sm,
                                      const slang::parsing::Token& token) {
    if (!token || !token.location().valid())
        return kInvalidSourceFileID;
    auto uri = uri_from_source_location(sm, token.location());
    if (uri.empty())
        return kInvalidSourceFileID;
    return index.intern_source_file(std::move(uri));
}

SourceFileID source_file_id_for_location(SyntaxIndex& index, const slang::SourceManager& sm,
                                         slang::SourceLocation location) {
    auto uri = uri_from_source_location(sm, location);
    if (uri.empty())
        return kInvalidSourceFileID;
    return index.intern_source_file(std::move(uri));
}

bool syntax_fragment_edge_is_wordlike(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$' || c == '`';
}

bool syntax_needs_space_between_fragments(std::string_view previous, std::string_view next) {
    if (previous.empty() || next.empty())
        return false;

    const char prev = previous.back();
    const char curr = next.front();

    // Raw token rendering deliberately strips trivia so it can preserve macro
    // spelling and avoid SyntaxNode::toString() expansion artifacts.  Restore
    // just the separators needed for readable declaration text:
    //
    //   bit signed
    //   virtual interface axi_if
    //   logic [`WIDTH-1:0]
    //   struct packed { ... }
    if (syntax_fragment_edge_is_wordlike(prev) && syntax_fragment_edge_is_wordlike(curr))
        return true;
    if (syntax_fragment_edge_is_wordlike(prev) && (curr == '[' || curr == '{'))
        return true;
    if (prev == ',')
        return true;
    return false;
}

std::optional<std::string> source_text_for_syntax_range(const slang::SourceManager& sm,
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

static bool same_source_range(slang::SourceRange lhs, slang::SourceRange rhs) {
    return lhs.start() == rhs.start() && lhs.end() == rhs.end();
}

std::string render_syntax_token_text(const slang::SourceManager& sm,
                                     const slang::parsing::Token& token,
                                     std::optional<slang::SourceRange>& last_macro_range) {
    if (sm.isMacroLoc(token.location())) {
        const auto expansion_range = sm.getExpansionRange(token.location());

        // A single macro invocation can expand to several syntax tokens.  Emit
        // the invocation text once so hovers/index metadata show the user's
        // spelling, not repeated preprocessor expansion tokens.
        if (last_macro_range && same_source_range(*last_macro_range, expansion_range))
            return {};
        last_macro_range = expansion_range;

        if (auto text = source_text_for_syntax_range(sm, expansion_range))
            return *text;
    } else {
        last_macro_range.reset();
    }

    return std::string(token.rawText());
}

std::string render_syntax_node_text(const slang::SourceManager& sm,
                                    const slang::syntax::SyntaxNode& node) {
    std::string text;
    std::optional<slang::SourceRange> last_macro_range;
    for (auto it = node.tokens_begin(); it != node.tokens_end(); ++it) {
        const auto token = *it;
        if (!token || token.isMissing())
            continue;

        const auto fragment = render_syntax_token_text(sm, token, last_macro_range);
        if (fragment.empty())
            continue;

        if (syntax_needs_space_between_fragments(text, fragment))
            text += ' ';
        text += fragment;
    }
    return trim_copy(std::move(text));
}

std::string symbol_canonical(std::string kind, std::string scope, std::string name) {
    if (scope.empty())
        return kind + "::" + name;
    return kind + "::" + scope + "::" + name;
}

bool is_module_value_kind(std::string_view kind) {
    return kind == "variable" || kind == "net" || kind == "parameter" || kind == "localparam" ||
           kind == "port";
}

std::string canonical_type_name_from_text(std::string_view type) {
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
    while (end > 0 && !syntax_fragment_edge_is_wordlike(type[end - 1]))
        --end;
    size_t begin = end;
    while (begin > 0 && syntax_fragment_edge_is_wordlike(type[begin - 1]))
        --begin;
    return std::string(type.substr(begin, end - begin));
}

namespace {

bool reference_type_char_is_wordlike(char c, bool allow_backtick) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$' ||
           (allow_backtick && c == '`');
}

std::string canonical_type_name_for_references(std::string_view type,
                                               ReferenceCollectionOptions options) {
    size_t end = type.size();
    while (end > 0 && !reference_type_char_is_wordlike(type[end - 1],
                                                       options.canonical_type_allows_backtick))
        --end;
    size_t begin = end;
    while (begin > 0 && reference_type_char_is_wordlike(type[begin - 1],
                                                        options.canonical_type_allows_backtick))
        --begin;
    return std::string(type.substr(begin, end - begin));
}

std::string simple_identifier_from_expr_node(const ExpressionSyntax* expr) {
    if (!expr)
        return {};
    if (const auto* ident = expr->as_if<IdentifierNameSyntax>())
        return std::string(ident->identifier.valueText());
    return {};
}

std::pair<int, int> token_pos(const slang::SourceManager& sm, const slang::parsing::Token& token) {
    if (!token || !token.location().valid())
        return {0, 0};
    const auto line = sm.getLineNumber(token.location());
    const auto col = sm.getColumnNumber(token.location());
    return {line > 0 ? static_cast<int>(line) : 0, col > 0 ? static_cast<int>(col) - 1 : 0};
}

void add_reference_entry(SyntaxIndex& index, std::string name, SourceFileID file_id,
                         std::string canonical_id, int line, int col) {
    if (name.empty())
        return;
    const auto end_col = col + static_cast<int>(name.size());
    index.references.push_back(ReferenceEntry{
        .name = std::move(name),
        .file_id = file_id,
        .symbol_id = SymbolID::from_canonical(canonical_id),
        .symbol_debug = std::move(canonical_id),
        .line = line,
        .col = col,
        .end_col = end_col,
    });
}

} // namespace

void collect_reference_occurrences(const SyntaxNode& root, SyntaxIndex& index,
                                   const slang::SourceManager& sm,
                                   ReferenceCollectionOptions options) {
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
            // A file can contain duplicate declarations while being edited.
            // Prefer the later syntactic type for member access resolution.
            module_value_types[key] = canonical_type_name_for_references(value.type, options);
        }
    }

    for (const auto& cls : index.classes) {
        const std::string class_scope =
            cls.parent_scope.empty() ? cls.name : cls.parent_scope + "::" + cls.name;
        const auto class_id = symbol_canonical("class", cls.parent_scope, cls.name);
        if (!ambiguous_type_names.contains(cls.name) &&
            !unique_type_ids.try_emplace(cls.name, class_id).second) {
            unique_type_ids.erase(cls.name);
            ambiguous_type_names.insert(cls.name);
        }
        for (const auto& field : cls.fields)
            class_fields.insert(class_scope + "\n" + field.name);
    }

    for (const auto& td : index.typedefs) {
        const auto typedef_id = symbol_canonical("typedef", td.parent_scope, td.name);
        if (!ambiguous_type_names.contains(td.name) &&
            !unique_type_ids.try_emplace(td.name, typedef_id).second) {
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
        add_reference_entry(index, std::string(token.valueText()),
                            source_file_id_for_token(index, sm, token),
                            std::move(canonical_id), line, col);
    };

    // Declarations are already extracted into the index.  Add owner-qualified
    // declaration references from those tables so closed-file reference search
    // can match e.g. `.clk(...)` to `module memory(input clk);` without loading
    // the declaring file's AST.
    for (const auto& module : index.modules) {
        if (!module.name.empty() && module.line > 0)
            add_reference_entry(index, module.name, module.file_id,
                                symbol_canonical("module", {}, module.name), module.line,
                                module.col);
        for (const auto& port : module.ports) {
            if (port.name.empty() || port.line <= 0)
                continue;
            const bool is_parameter =
                port.direction == "parameter" || port.direction == "localparam";
            add_reference_entry(index, port.name, port.file_id,
                                symbol_canonical(is_parameter ? "module_param" : "module_port",
                                                 module.name, port.name),
                                port.line, port.col);
        }
    }

    for (const auto& value : index.values) {
        if (value.name.empty() || value.parent_scope.empty() || !is_module_value_kind(value.kind) ||
            value.line <= 0 || index.package_names.contains(value.parent_scope))
            continue;
        add_reference_entry(index, value.name, value.file_id,
                            symbol_canonical("module_signal", value.parent_scope, value.name),
                            value.line, value.col);
    }
    for (const auto& value : index.values) {
        if (value.name.empty() || value.parent_scope.empty() ||
            !index.package_names.contains(value.parent_scope) || value.line <= 0)
            continue;
        add_reference_entry(index, value.name, value.file_id,
                            symbol_canonical("package_value", value.parent_scope, value.name),
                            value.line, value.col);
    }
    for (const auto& cls : index.classes) {
        if (cls.name.empty() || cls.line <= 0)
            continue;
        const auto class_scope =
            cls.parent_scope.empty() ? cls.name : cls.parent_scope + "::" + cls.name;
        add_reference_entry(index, cls.name, cls.file_id,
                            symbol_canonical("class", cls.parent_scope, cls.name), cls.line,
                            cls.col);
        for (const auto& field : cls.fields) {
            if (field.name.empty() || field.line <= 0)
                continue;
            add_reference_entry(index, field.name, field.file_id,
                                symbol_canonical("class_field", class_scope, field.name),
                                field.line, field.col);
        }
    }
    for (const auto& td : index.typedefs) {
        if (td.name.empty() || td.line <= 0)
            continue;
        const auto typedef_scope =
            td.parent_scope.empty() ? td.name : td.parent_scope + "::" + td.name;
        add_reference_entry(index, td.name, td.file_id,
                            symbol_canonical("typedef", td.parent_scope, td.name), td.line,
                            td.col);
        for (const auto& field : td.fields) {
            if (field.name.empty() || field.line <= 0)
                continue;
            add_reference_entry(index, field.name, field.file_id,
                                symbol_canonical("typedef_field", typedef_scope, field.name),
                                field.line, field.col);
        }
        for (const auto& member : td.enum_members) {
            if (member.name.empty() || member.line <= 0)
                continue;
            add_reference_entry(index, member.name, member.file_id,
                                symbol_canonical("enum_member", typedef_scope, member.name),
                                member.line, member.col);
        }
    }

    struct Visitor : SyntaxVisitor<Visitor> {
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

        Visitor(SyntaxIndex& index, const slang::SourceManager& sm,
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

        void handle(const ModuleDeclarationSyntax& node) {
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

        void handle(const ClassDeclarationSyntax& node) {
            const std::string class_name(node.name.valueText());
            const std::string parent_scope = !current_package.empty() ? current_package : std::string{};
            if (!class_name.empty())
                add_ref(node.name, symbol_canonical("class", parent_scope, class_name));

            auto previous_class = current_class;
            current_class = parent_scope.empty() ? class_name : parent_scope + "::" + class_name;
            visitDefault(node);
            current_class = std::move(previous_class);
        }

        void handle(const TypedefDeclarationSyntax& node) {
            const std::string typedef_name(node.name.valueText());
            const std::string parent_scope = !current_package.empty() ? current_package : std::string{};
            if (!typedef_name.empty())
                add_ref(node.name, symbol_canonical("typedef", parent_scope, typedef_name));
            visitDefault(node);
        }

        void handle(const HierarchyInstantiationSyntax& node) {
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

        void handle(const MemberAccessExpressionSyntax& node) {
            const std::string field_name(node.name.valueText());
            const std::string object_name = simple_identifier_from_expr_node(node.left);
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
            while (i > 0 && syntax_fragment_edge_is_wordlike(source[i - 1]))
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

    Visitor visitor(index, sm, add_reference, module_values, module_value_types, package_values,
                    class_fields, typedef_fields, unique_typedef_scopes, unique_enum_member_ids,
                    unique_type_ids);
    root.visit(visitor);
}
