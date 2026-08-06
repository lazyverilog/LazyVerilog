#pragma once

#include "string_utils.hpp"
#include "syntax_index.hpp"
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include <slang/parsing/Token.h>
#include <slang/syntax/SyntaxNode.h>
#include <slang/text/SourceLocation.h>

namespace slang {
class SourceManager;
namespace syntax {
class ExpressionSyntax;
class PropertyExprSyntax;
class SyntaxTree;
}
}

/// Convert a SourceManager file name to a URI.  slang may already store a URI
/// for in-memory buffers; real file paths are normalised through uri_from_path().
std::string uri_from_file_name(std::string_view file_name);

/// Convert a SourceManager location to a URI, returning an empty string for
/// invalid or non-file-backed locations.
std::string uri_from_source_location(const slang::SourceManager& sm,
                                     slang::SourceLocation location);

SourceFileID source_file_id_for_location(SyntaxIndex& index, const slang::SourceManager& sm,
                                         slang::SourceLocation location);

/// Per-index-build cache for SourceLocation buffer -> SourceFileID.
///
/// SourceManager lookups eventually normalize filesystem paths, and reference
/// indexing can ask for the source file of thousands of identifier tokens in a
/// single large RTL file.  The buffer id is stable for the lifetime of one
/// SyntaxTree / SourceManager, so callers that walk many tokens should use this
/// resolver instead of repeatedly normalizing the same path.  The cache is
/// intentionally transient: SyntaxIndex stores compact SourceFileIDs, not this
/// build-only helper.
///
/// The lookup is two-level because slang allocates a fresh BufferID for every
/// macro expansion, while every one of those buffers reports the same file
/// name.  Keying only on the buffer therefore misses on almost every token in
/// macro-heavy code and re-canonicalizes a path that always resolves to the
/// same file.  The name level absorbs those misses; the buffer level stays in
/// front of it because getFileName() itself walks the expansion chain under a
/// SourceManager lock.
class SourceFileIdResolver {
public:
    SourceFileID for_token(SyntaxIndex& index, const slang::SourceManager& sm,
                           const slang::parsing::Token& token);
    SourceFileID for_location(SyntaxIndex& index, const slang::SourceManager& sm,
                              slang::SourceLocation location);

private:
    // Transparent hash so the name lookup can be done on the string_view
    // returned by getFileName() without allocating a std::string per miss.
    struct TransparentStringHash {
        using is_transparent = void;
        size_t operator()(std::string_view text) const noexcept {
            return std::hash<std::string_view>{}(text);
        }
    };

    std::unordered_map<uint32_t, SourceFileID> by_buffer_;
    std::unordered_map<std::string, SourceFileID, TransparentStringHash, std::equal_to<>> by_name_;
};

/// Safely return a token's user-facing value text, or an empty string for a
/// missing token.  Keeping this helper shared prevents each indexer / feature
/// from making slightly different missing-token choices.
std::string token_value_text(const slang::parsing::Token& token);

bool syntax_fragment_edge_is_wordlike(char c);

/// Return a token position using slang's 1-based line numbers and LSP-style
/// 0-based columns.  This is the historical coordinate shape stored in
/// SyntaxIndex entries: callers convert the line to LSP coordinates at the
/// API boundary with to_lsp_line().
std::pair<int, int> token_pos_line1_col0(const slang::SourceManager& sm,
                                         const slang::parsing::Token& token);

/// Return a fully LSP-style token position: 0-based line and 0-based column.
/// Feature implementations that edit the current document use this form
/// directly because LSP text edits are already 0-based.
std::pair<int, int> token_pos_line0_col0(const slang::SourceManager& sm,
                                         const slang::parsing::Token& token);
bool syntax_needs_space_between_fragments(std::string_view previous, std::string_view next);
std::optional<std::string> source_text_for_syntax_range(const slang::SourceManager& sm,
                                                       slang::SourceRange range);
std::string render_syntax_token_text(const slang::SourceManager& sm,
                                     const slang::parsing::Token& token,
                                     std::optional<slang::SourceRange>& last_macro_range);
std::string render_syntax_node_text(const slang::SourceManager& sm,
                                    const slang::syntax::SyntaxNode& node);

std::string symbol_canonical(std::string_view kind, std::string_view scope, std::string_view name);
bool is_module_value_kind(std::string_view kind);
std::string canonical_type_name_from_text(std::string_view type);

/// Return the plain identifier name represented by a syntax expression, or an
/// empty string if the expression is more complex than a single identifier.
std::string simple_identifier_from_expr(const slang::syntax::ExpressionSyntax* expr);

/// Property-expression wrapper used by named port / parameter connections.
/// It intentionally accepts only the simple property -> simple sequence ->
/// identifier shape so callers do not accidentally treat arbitrary expressions
/// as connection signal names.
std::string simple_identifier_from_expr(const slang::syntax::PropertyExprSyntax* expr);
std::vector<std::string> collect_include_dependency_uris(const slang::SourceManager& sm,
                                                         const std::string& owning_uri);

/// Combined single-pass replacement for collect_reference_occurrences() and
/// collect_macro_reference_occurrences().  Performs one SyntaxTree traversal
/// instead of two by composing the macro-expansion visitToken() and the
/// reference-resolution visitToken() into a single CombinedVisitor.
/// The SubroutineDeclarationCollector pre-pass still runs as a separate walk
/// because its output (declared_subroutines) is consumed by the main visitor.
void collect_combined_occurrences(const slang::syntax::SyntaxTree& tree,
                                  const slang::syntax::SyntaxNode& root, SyntaxIndex& index,
                                  const slang::SourceManager& sm);
