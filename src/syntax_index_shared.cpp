#include "syntax_index_shared.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
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

std::vector<std::string> collect_include_dependency_uris(const slang::SourceManager& sm,
                                                         const std::string& owning_uri) {
    std::unordered_set<std::string> seen;
    std::vector<std::string> result;

    auto add_path = [&](const std::filesystem::path& path) {
        if (path.empty())
            return;
        const auto uri = uri_from_path(path);
        if (uri == owning_uri)
            return;
        if (seen.insert(uri).second)
            result.push_back(uri);
    };

    for (auto buffer : sm.getAllBuffers()) {
        const auto& full_path = sm.getFullPath(buffer);
        if (!full_path.empty()) {
            add_path(full_path);
            continue;
        }

        const auto raw_name = sm.getRawFileName(buffer);
        if (!raw_name.empty())
            add_path(std::filesystem::path(std::string(raw_name)));
    }

    return result;
}

namespace {

std::string canonical_type_name_for_references(std::string_view type) {
    // Keep macro-spelled type tokens intact for both closed-file and dynamic
    // indexes.  This is still textual canonicalization, not macro expansion,
    // but both indexing paths now use one wordlike definition so live-buffer
    // references cannot disagree with project shards only because a type was
    // written as `PACKET_T.
    size_t end = type.size();
    while (end > 0 && !syntax_fragment_edge_is_wordlike(type[end - 1]))
        --end;
    size_t begin = end;
    while (begin > 0 && syntax_fragment_edge_is_wordlike(type[begin - 1]))
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

slang::parsing::Token last_identifier_token(const SyntaxNode& node) {
    slang::parsing::Token result;
    for (auto it = node.tokens_begin(); it != node.tokens_end(); ++it) {
        const auto token = *it;
        if (token && token.kind == slang::parsing::TokenKind::Identifier)
            result = token;
    }
    return result;
}

enum class SubroutineOwnerKind {
    Unit,
    Module,
    Interface,
    Program,
    Package,
    Class,
    Checker,
};

std::string_view subroutine_owner_key(SubroutineOwnerKind kind) {
    switch (kind) {
    case SubroutineOwnerKind::Unit:
        return "unit";
    case SubroutineOwnerKind::Module:
        return "module";
    case SubroutineOwnerKind::Interface:
        return "interface";
    case SubroutineOwnerKind::Program:
        return "program";
    case SubroutineOwnerKind::Package:
        return "package";
    case SubroutineOwnerKind::Class:
        return "class";
    case SubroutineOwnerKind::Checker:
        return "checker";
    }
    return "unit";
}

std::string_view subroutine_symbol_kind(SubroutineOwnerKind owner_kind) {
    switch (owner_kind) {
    case SubroutineOwnerKind::Module:
        return "module_subroutine";
    case SubroutineOwnerKind::Interface:
        return "interface_subroutine";
    case SubroutineOwnerKind::Program:
        return "program_subroutine";
    case SubroutineOwnerKind::Package:
        return "package_subroutine";
    case SubroutineOwnerKind::Class:
        return "class_method";
    case SubroutineOwnerKind::Checker:
        return "checker_subroutine";
    case SubroutineOwnerKind::Unit:
        return "unit_subroutine";
    }
    return "unit_subroutine";
}

std::string subroutine_symbol_id(SubroutineOwnerKind owner_kind, std::string_view owner_name,
                                 std::string_view name) {
    const auto kind = subroutine_symbol_kind(owner_kind);
    if (owner_kind == SubroutineOwnerKind::Unit)
        return symbol_canonical(std::string(kind), {}, std::string(name));
    return symbol_canonical(std::string(kind), std::string(owner_name), std::string(name));
}

std::string subroutine_scope_key(SubroutineOwnerKind owner_kind, std::string_view owner_name,
                                 std::string_view name) {
    return std::string(subroutine_owner_key(owner_kind)) + "\n" + std::string(owner_name) +
           "\n" + std::string(name);
}

std::string final_name_from_qualified_name(std::string_view qualified_name) {
    const size_t last_scope = qualified_name.rfind("::");
    if (last_scope == std::string_view::npos)
        return std::string(qualified_name);
    return std::string(qualified_name.substr(last_scope + 2));
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
    std::unordered_set<std::string> declared_subroutines;

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
            module_value_types[key] = canonical_type_name_for_references(value.type);
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

    auto module_owner_kind = [](SyntaxKind kind) -> SubroutineOwnerKind {
        // slang represents module, interface, program, and package declarations
        // with ModuleDeclarationSyntax.  Keep their SymbolIDs distinct so a
        // package helper `foo`, an interface task `foo`, and a module task
        // `foo` do not collapse back to a textual name match.
        if (kind == SyntaxKind::PackageDeclaration)
            return SubroutineOwnerKind::Package;
        if (kind == SyntaxKind::InterfaceDeclaration)
            return SubroutineOwnerKind::Interface;
        if (kind == SyntaxKind::ProgramDeclaration)
            return SubroutineOwnerKind::Program;
        return SubroutineOwnerKind::Module;
    };

    auto resolve_subroutine_owner_from_qualified_name =
        [&](std::string_view qualified_name,
            SubroutineOwnerKind fallback_kind,
            std::string_view fallback_name) -> std::pair<SubroutineOwnerKind, std::string> {
        const size_t last_scope = qualified_name.rfind("::");
        if (last_scope == std::string_view::npos)
            return {fallback_kind, std::string(fallback_name)};

        const std::string owner(qualified_name.substr(0, last_scope));
        if (index.package_names.contains(owner))
            return {SubroutineOwnerKind::Package, owner};

        // Out-of-block class methods are commonly spelled `function C::f;`.
        // They are not lexically inside the class declaration, so recover the
        // class owner from the qualified prototype name.  If the qualifier is
        // package-qualified (`pkg::C::f`) keep the full qualifier as the class
        // method owner; this mirrors how class scopes are represented elsewhere
        // in the reference index.
        return {SubroutineOwnerKind::Class, owner};
    };

    struct SubroutineDeclarationCollector : SyntaxVisitor<SubroutineDeclarationCollector> {
        const slang::SourceManager& sm;
        std::unordered_set<std::string>& declared_subroutines;
        const decltype(module_owner_kind)& module_kind_fn;
        const decltype(resolve_subroutine_owner_from_qualified_name)& resolve_owner;
        SubroutineOwnerKind current_owner_kind{SubroutineOwnerKind::Unit};
        std::string current_owner_name;
        std::string current_package;
        std::string current_class;

        SubroutineDeclarationCollector(
            const slang::SourceManager& sm,
            std::unordered_set<std::string>& declared_subroutines,
            const decltype(module_owner_kind)& module_owner_kind,
            const decltype(resolve_subroutine_owner_from_qualified_name)& resolve_owner)
            : sm(sm), declared_subroutines(declared_subroutines),
              module_kind_fn(module_owner_kind), resolve_owner(resolve_owner) {}

        void handle(const ModuleDeclarationSyntax& node) {
            const auto previous_kind = current_owner_kind;
            const auto previous_name = current_owner_name;
            const auto previous_package = current_package;

            current_owner_kind = module_kind_fn(node.kind);
            current_owner_name = std::string(node.header->name.valueText());
            if (node.kind == SyntaxKind::PackageDeclaration)
                current_package = current_owner_name;

            visitDefault(node);

            current_owner_kind = previous_kind;
            current_owner_name = previous_name;
            current_package = previous_package;
        }

        void handle(const CheckerDeclarationSyntax& node) {
            const auto previous_kind = current_owner_kind;
            const auto previous_name = current_owner_name;
            current_owner_kind = SubroutineOwnerKind::Checker;
            current_owner_name = std::string(node.name.valueText());
            visitDefault(node);
            current_owner_kind = previous_kind;
            current_owner_name = previous_name;
        }

        void handle(const ClassDeclarationSyntax& node) {
            const auto previous_kind = current_owner_kind;
            const auto previous_name = current_owner_name;
            const auto previous_class = current_class;

            const std::string class_name(node.name.valueText());
            if (!current_class.empty())
                current_class += "::" + class_name;
            else if (!current_package.empty())
                current_class = current_package + "::" + class_name;
            else
                current_class = class_name;

            current_owner_kind = SubroutineOwnerKind::Class;
            current_owner_name = current_class;
            visitDefault(node);

            current_owner_kind = previous_kind;
            current_owner_name = previous_name;
            current_class = previous_class;
        }

        void handle(const FunctionDeclarationSyntax& node) {
            if (!node.prototype || !node.prototype->name)
                return;

            const auto qualified_name = render_syntax_node_text(sm, *node.prototype->name);
            const auto name = final_name_from_qualified_name(qualified_name);
            if (!name.empty()) {
                auto [owner_kind, owner_name] =
                    resolve_owner(qualified_name, current_owner_kind, current_owner_name);
                declared_subroutines.insert(subroutine_scope_key(owner_kind, owner_name, name));
            }

            visitDefault(node);
        }
    };

    SubroutineDeclarationCollector subroutines(sm, declared_subroutines, module_owner_kind,
                                               resolve_subroutine_owner_from_qualified_name);
    root.visit(subroutines);

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
        const std::unordered_set<std::string>& declared_subroutines;
        const decltype(module_owner_kind)& module_kind_fn;
        const decltype(resolve_subroutine_owner_from_qualified_name)& resolve_subroutine_owner;
        std::string current_module;
        SubroutineOwnerKind current_module_kind{SubroutineOwnerKind::Module};
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
                const std::unordered_map<std::string, std::string>& unique_type_ids,
                const std::unordered_set<std::string>& declared_subroutines,
                const decltype(module_owner_kind)& module_owner_kind,
                const decltype(resolve_subroutine_owner_from_qualified_name)& resolve_subroutine_owner)
            : index(index), sm(sm), add_ref(add_reference), module_values(module_values),
              module_value_types(module_value_types), package_values(package_values),
              class_fields(class_fields), typedef_fields(typedef_fields),
              unique_typedef_scopes(unique_typedef_scopes),
              unique_enum_member_ids(unique_enum_member_ids), unique_type_ids(unique_type_ids),
              declared_subroutines(declared_subroutines), module_kind_fn(module_owner_kind),
              resolve_subroutine_owner(resolve_subroutine_owner) {}

        void handle(const ModuleDeclarationSyntax& node) {
            const std::string module_name(node.header->name.valueText());
            if (!module_name.empty())
                add_ref(node.header->name, symbol_canonical("module", {}, module_name));

            auto previous_module = current_module;
            auto previous_module_kind = current_module_kind;
            auto previous_package = current_package;
            if (node.kind == SyntaxKind::PackageDeclaration) {
                current_package = module_name;
                current_module.clear();
                current_module_kind = SubroutineOwnerKind::Module;
            } else {
                current_module = module_name;
                current_module_kind = module_kind_fn(node.kind);
            }
            visitDefault(node);
            current_module = std::move(previous_module);
            current_module_kind = std::move(previous_module_kind);
            current_package = std::move(previous_package);
        }

        void handle(const CheckerDeclarationSyntax& node) {
            auto previous_module = current_module;
            auto previous_module_kind = current_module_kind;
            current_module = std::string(node.name.valueText());
            current_module_kind = SubroutineOwnerKind::Checker;
            visitDefault(node);
            current_module = std::move(previous_module);
            current_module_kind = std::move(previous_module_kind);
        }

        void handle(const ClassDeclarationSyntax& node) {
            const std::string class_name(node.name.valueText());
            const std::string parent_scope = !current_package.empty() ? current_package : std::string{};
            if (!class_name.empty())
                add_ref(node.name, symbol_canonical("class", parent_scope, class_name));

            auto previous_class = current_class;
            if (!current_class.empty())
                current_class += "::" + class_name;
            else
                current_class = parent_scope.empty() ? class_name : parent_scope + "::" + class_name;
            visitDefault(node);
            current_class = std::move(previous_class);
        }

        std::optional<std::string> subroutine_id_for_unqualified_name(std::string_view name) const {
            // SystemVerilog subroutines live in several lexical owners:
            // compilation-unit, packages, modules/interfaces/programs,
            // checkers, and classes.  Closed-file references cannot ask slang's
            // semantic model which declaration a call bound to, but they can
            // safely use a syntactic owner when the declaration and call are in
            // the same lexical owner.  This prevents weak `name:<id>` matches
            // from merging unrelated local tasks/functions with the same name.
            if (!current_class.empty()) {
                const auto key = subroutine_scope_key(SubroutineOwnerKind::Class, current_class, name);
                if (declared_subroutines.contains(key))
                    return subroutine_symbol_id(SubroutineOwnerKind::Class, current_class, name);
            }
            if (!current_module.empty()) {
                const auto key = subroutine_scope_key(current_module_kind, current_module, name);
                if (declared_subroutines.contains(key))
                    return subroutine_symbol_id(current_module_kind, current_module, name);
            }
            if (!current_package.empty()) {
                const auto key = subroutine_scope_key(SubroutineOwnerKind::Package, current_package, name);
                if (declared_subroutines.contains(key))
                    return subroutine_symbol_id(SubroutineOwnerKind::Package, current_package, name);
            }

            const auto key = subroutine_scope_key(SubroutineOwnerKind::Unit, {}, name);
            if (declared_subroutines.contains(key))
                return subroutine_symbol_id(SubroutineOwnerKind::Unit, {}, name);
            return std::nullopt;
        }

        std::optional<std::string> subroutine_id_for_invocation_name(
            std::string_view rendered_name) const {
            const size_t last_scope = rendered_name.rfind("::");
            if (last_scope == std::string_view::npos)
                return subroutine_id_for_unqualified_name(rendered_name);

            const auto callee = final_name_from_qualified_name(rendered_name);
            auto [owner_kind, owner_name] =
                resolve_subroutine_owner(rendered_name, SubroutineOwnerKind::Unit, std::string_view{});
            const auto key = subroutine_scope_key(owner_kind, owner_name, callee);
            if (!declared_subroutines.contains(key))
                return std::nullopt;
            return subroutine_symbol_id(owner_kind, owner_name, callee);
        }

        void handle(const FunctionDeclarationSyntax& node) {
            if (node.prototype && node.prototype->name) {
                const auto qualified_name = render_syntax_node_text(sm, *node.prototype->name);
                const auto name_token = last_identifier_token(*node.prototype->name);
                if (name_token) {
                    const auto name = std::string(name_token.valueText());
                    auto [owner_kind, owner_name] =
                        resolve_subroutine_owner(
                            qualified_name,
                            current_class.empty()
                                ? (current_module.empty()
                                       ? (current_package.empty() ? SubroutineOwnerKind::Unit
                                                                  : SubroutineOwnerKind::Package)
                                       : current_module_kind)
                                : SubroutineOwnerKind::Class,
                            current_class.empty()
                                ? (current_module.empty() ? std::string_view(current_package)
                                                          : std::string_view(current_module))
                                : std::string_view(current_class));
                    add_ref(name_token, subroutine_symbol_id(owner_kind, owner_name, name));
                }
            }
            visitDefault(node);
        }

        void handle(const InvocationExpressionSyntax& node) {
            if (node.left) {
                const auto name_token = last_identifier_token(*node.left);
                if (name_token) {
                    const auto rendered_name = render_syntax_node_text(sm, *node.left);
                    if (auto id = subroutine_id_for_invocation_name(rendered_name))
                        add_ref(name_token, *id);
                }
            }
            visitDefault(node);
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
                    unique_type_ids, declared_subroutines, module_owner_kind,
                    resolve_subroutine_owner_from_qualified_name);
    root.visit(visitor);
}
