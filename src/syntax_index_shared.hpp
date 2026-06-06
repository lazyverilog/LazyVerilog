#pragma once

#include "string_utils.hpp"
#include "syntax_index.hpp"
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

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

SourceFileID source_file_id_for_token(SyntaxIndex& index, const slang::SourceManager& sm,
                                      const slang::parsing::Token& token);
SourceFileID source_file_id_for_location(SyntaxIndex& index, const slang::SourceManager& sm,
                                         slang::SourceLocation location);

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

std::string symbol_canonical(std::string kind, std::string scope, std::string name);
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

/// Add declaration and use-site ReferenceEntry records to an already-populated
/// SyntaxIndex.  Both closed-file indexing and dynamic/open-buffer indexing call
/// this shared implementation so new symbol kinds cannot diverge between the
/// two paths.
void collect_reference_occurrences(const slang::syntax::SyntaxNode& root, SyntaxIndex& index,
                                   const slang::SourceManager& sm);

/// Add macro declaration and invocation ReferenceEntry records.  Macro
/// definitions are preprocessor facts rather than normal SyntaxNode children,
/// so this helper accepts the whole SyntaxTree.
void collect_macro_reference_occurrences(const slang::syntax::SyntaxTree& tree, SyntaxIndex& index);
