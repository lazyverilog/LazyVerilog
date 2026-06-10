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


SourceFileID SourceFileIdResolver::for_token(SyntaxIndex& index, const slang::SourceManager& sm,
                                             const slang::parsing::Token& token) {
    if (!token || !token.location().valid())
        return kInvalidSourceFileID;
    return for_location(index, sm, token.location());
}

SourceFileID SourceFileIdResolver::for_location(SyntaxIndex& index, const slang::SourceManager& sm,
                                                slang::SourceLocation location) {
    if (!location.valid())
        return kInvalidSourceFileID;

    const auto buffer_id = location.buffer().getId();
    if (auto it = by_buffer_.find(buffer_id); it != by_buffer_.end())
        return it->second;

    auto uri = uri_from_source_location(sm, location);
    if (uri.empty()) {
        by_buffer_.emplace(buffer_id, kInvalidSourceFileID);
        return kInvalidSourceFileID;
    }

    const auto file_id = index.intern_source_file(std::move(uri));
    by_buffer_.emplace(buffer_id, file_id);
    return file_id;
}

std::string token_value_text(const slang::parsing::Token& token) {
    return token ? std::string(token.valueText()) : std::string{};
}

std::pair<int, int> token_pos_line1_col0(const slang::SourceManager& sm,
                                         const slang::parsing::Token& token) {
    if (!token || !token.location().valid())
        return {0, 0};

    const auto line = sm.getLineNumber(token.location());
    const auto col = sm.getColumnNumber(token.location());
    return {line > 0 ? static_cast<int>(line) : 0,
            col > 0 ? static_cast<int>(col) - 1 : 0};
}

std::pair<int, int> token_pos_line0_col0(const slang::SourceManager& sm,
                                         const slang::parsing::Token& token) {
    auto [line, col] = token_pos_line1_col0(sm, token);
    return {line > 0 ? line - 1 : 0, col};
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

std::string symbol_canonical(std::string_view kind, std::string_view scope, std::string_view name) {
    std::string result;
    if (scope.empty()) {
        result.reserve(kind.size() + 2 + name.size());
        result = kind;
        result += "::";
        result += name;
    } else {
        result.reserve(kind.size() + 2 + scope.size() + 2 + name.size());
        result = kind;
        result += "::";
        result += scope;
        result += "::";
        result += name;
    }
    return result;
}

bool is_module_value_kind(std::string_view kind) {
    return kind == "variable" || kind == "net" || kind == "parameter" || kind == "localparam" ||
           kind == "port";
}


std::string simple_identifier_from_expr(const slang::syntax::ExpressionSyntax* expr) {
    if (!expr)
        return {};
    if (const auto* ident = expr->as_if<IdentifierNameSyntax>())
        return std::string(ident->identifier.valueText());
    return {};
}

std::string simple_identifier_from_expr(const slang::syntax::PropertyExprSyntax* expr) {
    if (!expr)
        return {};
    if (const auto* prop = expr->as_if<SimplePropertyExprSyntax>()) {
        if (const auto* seq = prop->expr->as_if<SimpleSequenceExprSyntax>())
            return simple_identifier_from_expr(seq->expr);
    }
    return {};
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

    // Find the owning file's BufferID so we can distinguish its real `include
    // children from open-buffer overlays that were pre-loaded into the same
    // SourceManager.  Overlays are injected via assignText() with no
    // includedFrom, making them indistinguishable from root files without this
    // anchor check.
    slang::BufferID owning_buffer;
    for (auto buffer : sm.getAllBuffers()) {
        const auto& full_path = sm.getFullPath(buffer);
        if (!full_path.empty() && uri_from_path(full_path) == owning_uri) {
            owning_buffer = buffer;
            break;
        }
    }
    if (!owning_buffer.valid())
        return result;

    // For each buffer, walk the includedFrom chain to its root.  Only buffers
    // whose chain roots at owning_buffer are genuine `include dependencies.
    // Buffers with no includedFrom (overlays, unrelated roots) are skipped.
    for (auto buffer : sm.getAllBuffers()) {
        const auto loc = sm.getIncludedFrom(buffer);
        if (!loc.valid())
            continue;  // root buffer — owning file or a pre-loaded overlay

        // Walk to the root of the include chain.
        slang::BufferID current = buffer;
        while (true) {
            const auto parent = sm.getIncludedFrom(current);
            if (!parent.valid())
                break;
            current = parent.buffer();
        }
        if (current != owning_buffer)
            continue;  // rooted at an overlay, not the owning file

        const auto& full_path = sm.getFullPath(buffer);
        if (full_path.empty())
            continue;
        const auto uri = uri_from_path(full_path);
        if (uri == owning_uri)
            continue;
        if (seen.insert(uri).second)
            result.push_back(uri);
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


void collect_combined_occurrences(const slang::syntax::SyntaxTree& tree,
                                  const slang::syntax::SyntaxNode& root, SyntaxIndex& index,
                                  const slang::SourceManager& sm) {
    // === Single shared SourceFileIdResolver ===
    SourceFileIdResolver file_ids;

    // === Lookup tables (from collect_reference_occurrences) ===
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
    std::unordered_map<std::string, std::string> package_type_ids;
    std::unordered_map<std::string, SourceFileID> declared_subroutines;

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
            module_value_types[key] = canonical_type_name_for_references(value.type);
        }
    }

    for (const auto& cls : index.classes) {
        const std::string class_scope =
            cls.parent_scope.empty() ? cls.name : cls.parent_scope + "::" + cls.name;
        const auto class_id = symbol_canonical("class", cls.parent_scope, cls.name);
        if (!cls.parent_scope.empty() && index.package_names.contains(cls.parent_scope)) {
            package_type_ids.try_emplace(cls.parent_scope + "\n" + cls.name, class_id);
        } else if (!ambiguous_type_names.contains(cls.name) &&
                   !unique_type_ids.try_emplace(cls.name, class_id).second) {
            unique_type_ids.erase(cls.name);
            ambiguous_type_names.insert(cls.name);
        }
        for (const auto& field : cls.fields)
            class_fields.insert(class_scope + "\n" + field.name);
    }

    for (const auto& td : index.typedefs) {
        const auto typedef_id = symbol_canonical("typedef", td.parent_scope, td.name);
        if (!td.parent_scope.empty() && index.package_names.contains(td.parent_scope)) {
            package_type_ids.try_emplace(td.parent_scope + "\n" + td.name, typedef_id);
        } else if (!ambiguous_type_names.contains(td.name) &&
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
                            file_ids.for_token(index, sm, token),
                            std::move(canonical_id), line, col);
    };

    auto module_owner_kind = [](SyntaxKind kind) -> SubroutineOwnerKind {
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
        return {SubroutineOwnerKind::Class, owner};
    };

    // Subroutine declarations are collected by the main CombinedVisitor below.
    // Calls that appear before their declaration are stored as deferred
    // invocations and resolved after the visitor finishes, so we no longer need
    // a separate full-tree SubroutineDeclarationCollector pre-pass here.

    // === Declaration reference injection ===
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

    // === Macro preamble ===
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

    // Macro declarations from the preprocessor table (not a tree walk)
    for (const auto* def : tree.getDefinedMacros()) {
        if (!def || !def->name || !def->name.location().valid() ||
            !sm.isFileLoc(def->name.location()))
            continue;
        const auto [line, col] = token_pos_line1_col0(sm, def->name);
        add_macro_ref(std::string(def->name.valueText()), file_ids.for_token(index, sm, def->name),
                      line, col);
    }

    // === Combined visitor ===
    struct CombinedVisitor : SyntaxVisitor<CombinedVisitor> {
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
        const std::unordered_map<std::string, std::string>& package_type_ids;
        std::unordered_map<std::string, SourceFileID>& declared_subroutines;
        const decltype(module_owner_kind)& module_kind_fn;
        const decltype(resolve_subroutine_owner_from_qualified_name)& resolve_subroutine_owner;
        std::string current_module;
        SubroutineOwnerKind current_module_kind{SubroutineOwnerKind::Module};
        std::string current_package;
        std::string current_class;

        struct TypeImport {
            std::string package_name;
            std::string symbol_name;
            bool wildcard{false};
        };
        std::vector<TypeImport> visible_type_imports;
        std::string scope_key; // reused buffer for lookup key construction

        struct DeferredInvocation {
            slang::parsing::Token name_token;
            std::string rendered_name;
            std::string class_ctx;
            std::string module_ctx;
            SubroutineOwnerKind module_kind_ctx{SubroutineOwnerKind::Module};
            std::string package_ctx;
        };
        std::vector<DeferredInvocation> deferred_invocations;

        // Extra fields for macro visitor
        decltype(add_macro_ref)& add_macro;
        SourceFileIdResolver& file_ids;
        // Per-buffer classification: true = macro definition buffer, false = file buffer.
        // Populated on first token seen from each buffer so isMacroLoc is called only once per buffer.
        std::unordered_map<uint32_t, bool> buffer_is_macro;
        // Deduplication for macro expansion sites. Key = (expansion_buf_id << 32) | expansion_offset.
        // Avoids recording the same macro invocation multiple times when it expands to many tokens.
        std::unordered_set<uint64_t> seen_expansions;

        CombinedVisitor(SyntaxIndex& index, const slang::SourceManager& sm,
                        decltype(add_reference)& add_reference,
                        const std::unordered_set<std::string>& module_values,
                        const std::unordered_map<std::string, std::string>& module_value_types,
                        const std::unordered_set<std::string>& package_values,
                        const std::unordered_set<std::string>& class_fields,
                        const std::unordered_set<std::string>& typedef_fields,
                        const std::unordered_map<std::string, std::string>& unique_typedef_scopes,
                        const std::unordered_map<std::string, std::string>& unique_enum_member_ids,
                        const std::unordered_map<std::string, std::string>& unique_type_ids,
                        const std::unordered_map<std::string, std::string>& package_type_ids,
                        std::unordered_map<std::string, SourceFileID>& declared_subroutines,
                        const decltype(module_owner_kind)& module_owner_kind,
                        const decltype(resolve_subroutine_owner_from_qualified_name)& resolve_subroutine_owner,
                        decltype(add_macro_ref)& add_macro_ref,
                        SourceFileIdResolver& file_ids)
            : index(index), sm(sm), add_ref(add_reference), module_values(module_values),
              module_value_types(module_value_types), package_values(package_values),
              class_fields(class_fields), typedef_fields(typedef_fields),
              unique_typedef_scopes(unique_typedef_scopes),
              unique_enum_member_ids(unique_enum_member_ids), unique_type_ids(unique_type_ids),
              package_type_ids(package_type_ids), declared_subroutines(declared_subroutines),
              module_kind_fn(module_owner_kind),
              resolve_subroutine_owner(resolve_subroutine_owner),
              add_macro(add_macro_ref), file_ids(file_ids) {}

        void handle(const ModuleDeclarationSyntax& node) {
            const std::string module_name(node.header->name.valueText());
            if (!module_name.empty())
                add_ref(node.header->name, symbol_canonical("module", {}, module_name));

            auto previous_module = current_module;
            auto previous_module_kind = current_module_kind;
            auto previous_package = current_package;
            const auto import_stack_size = visible_type_imports.size();
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
            visible_type_imports.resize(import_stack_size);
        }

        void handle(const CheckerDeclarationSyntax& node) {
            auto previous_module = current_module;
            auto previous_module_kind = current_module_kind;
            const auto import_stack_size = visible_type_imports.size();
            current_module = std::string(node.name.valueText());
            current_module_kind = SubroutineOwnerKind::Checker;
            visitDefault(node);
            current_module = std::move(previous_module);
            current_module_kind = std::move(previous_module_kind);
            visible_type_imports.resize(import_stack_size);
        }

        void handle(const ClassDeclarationSyntax& node) {
            const std::string class_name(node.name.valueText());
            const std::string parent_scope = !current_package.empty() ? current_package : std::string{};
            if (!class_name.empty())
                add_ref(node.name, symbol_canonical("class", parent_scope, class_name));

            auto previous_class = current_class;
            const auto import_stack_size = visible_type_imports.size();
            if (!current_class.empty())
                current_class += "::" + class_name;
            else
                current_class = parent_scope.empty() ? class_name : parent_scope + "::" + class_name;
            visitDefault(node);
            current_class = std::move(previous_class);
            visible_type_imports.resize(import_stack_size);
        }

        std::optional<std::string> subroutine_id_for_unqualified_name(std::string_view name) const {
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
                    declared_subroutines.insert_or_assign(
                        subroutine_scope_key(owner_kind, owner_name, name),
                        file_ids.for_token(index, sm, name_token));
                    add_ref(name_token, subroutine_symbol_id(owner_kind, owner_name, name));
                }
            }
            visitDefault(node);
        }

        void add_subroutine_invocation_ref(const slang::parsing::Token& name_token,
                                           std::string_view rendered_name,
                                           std::string_view module_ctx,
                                           SubroutineOwnerKind module_kind_ctx,
                                           const std::string& resolved_id) {
            add_ref(name_token, resolved_id);

            // If the subroutine resolved to a non-unit scope (module/interface/
            // program/etc.) but its declaration came from a different source
            // file than the call site, also emit a unit_subroutine SymbolID for
            // the call.  This is the important include-file bridge:
            //
            //   // params.svh, opened standalone by the user
            //   task add_number(); endtask        -> unit_subroutine::add_number
            //
            //   // memory_top.sv project shard
            //   `include "params.svh"             -> declaration token may be
            //                                        indexed as memory_top-local
            //   initial add_number();             -> module_subroutine::memory_top::add_number
            //
            // A find-references request started from params.svh uses the unit
            // SymbolID.  Without this extra occurrence on the call token, the
            // compact closed-file shard cannot match the call without an AST
            // re-walk.  Apply this to both immediate and deferred invocations;
            // the original c63ba94 fix only covered the deferred path, so calls
            // after their included declaration still missed this bridge ID.
            if (module_ctx.empty())
                return;

            const auto callee = final_name_from_qualified_name(rendered_name);
            const auto mkey = subroutine_scope_key(module_kind_ctx, module_ctx, callee);
            auto dit = declared_subroutines.find(mkey);
            if (dit == declared_subroutines.end())
                return;

            const auto call_fid = file_ids.for_token(index, sm, name_token);
            if (dit->second == call_fid || dit->second == kInvalidSourceFileID ||
                call_fid == kInvalidSourceFileID)
                return;

            const auto unit_id = subroutine_symbol_id(SubroutineOwnerKind::Unit, {}, callee);
            if (unit_id != resolved_id)
                add_ref(name_token, unit_id);
        }

        void handle(const InvocationExpressionSyntax& node) {
            if (node.left) {
                slang::parsing::Token name_token;
                std::string rendered_name;
                // Fast path: simple function call foo(...) — avoid token iteration.
                if (const auto* iname = node.left->as_if<IdentifierNameSyntax>()) {
                    name_token = iname->identifier;
                    if (name_token)
                        rendered_name = std::string(name_token.valueText());
                } else {
                    name_token = last_identifier_token(*node.left);
                    if (name_token)
                        rendered_name = render_syntax_node_text(sm, *node.left);
                }
                if (name_token && !rendered_name.empty()) {
                    if (auto id = subroutine_id_for_invocation_name(rendered_name)) {
                        add_subroutine_invocation_ref(name_token, rendered_name, current_module,
                                                      current_module_kind, *id);
                    } else {
                        deferred_invocations.push_back({name_token, std::move(rendered_name),
                                                        current_class, current_module,
                                                        current_module_kind, current_package});
                    }
                }
            }
            visitDefault(node);
        }

        void resolve_deferred_invocations() {
            for (auto& inv : deferred_invocations) {
                current_class = inv.class_ctx;
                current_module = inv.module_ctx;
                current_module_kind = inv.module_kind_ctx;
                current_package = inv.package_ctx;
                if (auto id = subroutine_id_for_invocation_name(inv.rendered_name))
                    add_subroutine_invocation_ref(inv.name_token, inv.rendered_name,
                                                  inv.module_ctx, inv.module_kind_ctx, *id);
            }
            deferred_invocations.clear();
        }

        void handle(const PackageImportDeclarationSyntax& node) {
            for (const auto* item : node.items) {
                if (!item)
                    continue;
                TypeImport import;
                import.package_name = std::string(item->package.valueText());
                import.wildcard = item->item.kind == slang::parsing::TokenKind::Star;
                if (!import.wildcard)
                    import.symbol_name = std::string(item->item.valueText());
                if (!import.package_name.empty())
                    visible_type_imports.push_back(std::move(import));
            }
            visitDefault(node);
        }

        std::optional<std::string> imported_type_id_for_unqualified_name(
            std::string_view name) const {
            for (auto it = visible_type_imports.rbegin(); it != visible_type_imports.rend(); ++it) {
                if (!it->wildcard && it->symbol_name != name)
                    continue;
                const auto type_it =
                    package_type_ids.find(it->package_name + "\n" + std::string(name));
                if (type_it != package_type_ids.end())
                    return type_it->second;
            }
            return std::nullopt;
        }

        void handle(const TypedefDeclarationSyntax& node) {
            const std::string typedef_name(node.name.valueText());
            const std::string parent_scope = !current_package.empty() ? current_package : std::string{};
            if (!typedef_name.empty())
                add_ref(node.name, symbol_canonical("typedef", parent_scope, typedef_name));
            visitDefault(node);
        }

        void handle(const ScopedNameSyntax& node) {
            if (!node.left || !node.right) { visitDefault(node); return; }

            // Fast path: scope::name where both sides are simple identifiers.
            // Avoids token iteration and string construction in render_syntax_node_text.
            if (const auto* lname = node.left->as_if<IdentifierNameSyntax>()) {
                if (const auto* rname = node.right->as_if<IdentifierNameSyntax>()) {
                    const std::string_view scope_sv = lname->identifier.valueText();
                    const std::string_view name_sv = rname->identifier.valueText();
                    if (!scope_sv.empty() && !name_sv.empty()) {
                        scope_key = scope_sv;
                        scope_key += '\n';
                        scope_key.append(name_sv);
                        if (const auto it = package_type_ids.find(scope_key);
                            it != package_type_ids.end()) {
                            add_ref(rname->identifier, it->second);
                            node.left->visit(*this);
                            return;
                        }
                    }
                    visitDefault(node);
                    return;
                }
            }

            // Slow path: complex names (e.g., pkg::sub::Type).
            const auto scope = render_syntax_node_text(sm, *node.left);
            const auto name_token = last_identifier_token(*node.right);
            if (!scope.empty() && name_token) {
                const std::string name(name_token.valueText());
                if (const auto it = package_type_ids.find(scope + "\n" + name);
                    it != package_type_ids.end()) {
                    add_ref(name_token, it->second);
                    node.left->visit(*this);
                    return;
                }
            }
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
            const std::string object_name = simple_identifier_from_expr(node.left);
            if (!current_module.empty() && !field_name.empty() && !object_name.empty()) {
                scope_key = current_module;
                scope_key += '\n';
                scope_key += object_name;
                const auto value_it = module_value_types.find(scope_key);
                if (value_it != module_value_types.end()) {
                    std::string typedef_scope = value_it->second;
                    if (const auto scope_it = unique_typedef_scopes.find(typedef_scope);
                        scope_it != unique_typedef_scopes.end())
                        typedef_scope = scope_it->second;
                    scope_key = typedef_scope;
                    scope_key += '\n';
                    scope_key += field_name;
                    if (typedef_fields.contains(scope_key)) {
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
            // Check the cheap module-context guard before the expensive source-text scan.
            if (current_module.empty() || field_name.empty() || module_value_types.empty())
                return false;
            const auto object_name = object_before_member_dot(token);
            if (object_name.empty())
                return false;
            scope_key = current_module;
            scope_key += '\n';
            scope_key += object_name;
            const auto value_it = module_value_types.find(scope_key);
            if (value_it == module_value_types.end())
                return false;
            std::string typedef_scope = value_it->second;
            if (const auto scope_it = unique_typedef_scopes.find(typedef_scope);
                scope_it != unique_typedef_scopes.end())
                typedef_scope = scope_it->second;
            scope_key = typedef_scope;
            scope_key += '\n';
            scope_key.append(field_name);
            if (!typedef_fields.contains(scope_key))
                return false;
            add_ref(token, symbol_canonical("typedef_field", typedef_scope, std::string(field_name)));
            return true;
        }

        void visitToken(slang::parsing::Token token) {
            if (!token || !token.location().valid())
                return;

            // Classify buffer on first encounter (saves repeated isMacroLoc calls).
            const uint32_t buf_id = token.location().buffer().getId();
            auto [it, inserted] = buffer_is_macro.emplace(buf_id, false);
            if (inserted) {
                it->second = sm.isMacroLoc(token.location());
            }

            if (it->second) {
                // Macro token — record the macro invocation once per expansion site.
                const auto range = sm.getExpansionRange(token.location());
                if (!range.start().valid())
                    return;
                const uint64_t key = (static_cast<uint64_t>(range.start().buffer().getId()) << 32) |
                                     static_cast<uint64_t>(range.start().offset());
                if (!seen_expansions.insert(key).second)
                    return; // already recorded this expansion site
                const auto macro_name = sm.getMacroName(token.location());
                if (macro_name.empty())
                    return;
                const int line_num = (int)sm.getLineNumber(range.start());
                int col = (int)sm.getColumnNumber(range.start()) - 1;
                if (col < 0)
                    col = 0;
                if (auto text = source_text_for_syntax_range(sm, range);
                    text && text->starts_with('`'))
                    ++col;
                add_macro(std::string(macro_name),
                          file_ids.for_location(index, sm, range.start()),
                          line_num, col);
                return;
            }
            // Reference fallback
            if (!token || token.kind != slang::parsing::TokenKind::Identifier ||
                !token.location().valid())
                return;
            const std::string name(token.valueText());
            if (name.empty())
                return;
            if (try_add_typedef_field_reference(token, name))
                return;
            if (!current_class.empty()) {
                scope_key = current_class;
                scope_key += '\n';
                scope_key += name;
                if (class_fields.contains(scope_key)) {
                    add_ref(token, symbol_canonical("class_field", current_class, name));
                    return;
                }
            }
            if (!current_module.empty()) {
                scope_key = current_module;
                scope_key += '\n';
                scope_key += name;
                if (module_values.contains(scope_key)) {
                    add_ref(token, symbol_canonical("module_signal", current_module, name));
                    return;
                }
            }
            if (!current_package.empty()) {
                scope_key = current_package;
                scope_key += '\n';
                scope_key += name;
                if (package_values.contains(scope_key)) {
                    add_ref(token, symbol_canonical("package_value", current_package, name));
                    return;
                }
                if (const auto type_it = package_type_ids.find(scope_key);
                    type_it != package_type_ids.end()) {
                    add_ref(token, type_it->second);
                    return;
                }
            }
            if (auto imported_id = imported_type_id_for_unqualified_name(name)) {
                add_ref(token, *imported_id);
                return;
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
            scope_key = "name:";
            scope_key += name;
            add_ref(token, scope_key);
        }
    };

    CombinedVisitor visitor(index, sm, add_reference, module_values, module_value_types,
                            package_values, class_fields, typedef_fields, unique_typedef_scopes,
                            unique_enum_member_ids, unique_type_ids, package_type_ids,
                            declared_subroutines, module_owner_kind,
                            resolve_subroutine_owner_from_qualified_name,
                            add_macro_ref, file_ids);
    root.visit(visitor);
    visitor.resolve_deferred_invocations();
}
