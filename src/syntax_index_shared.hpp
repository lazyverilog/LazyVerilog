#pragma once

#include "string_utils.hpp"
#include "syntax_index.hpp"
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include <slang/parsing/Token.h>
#include <slang/syntax/SyntaxNode.h>
#include <slang/text/SourceLocation.h>

namespace slang {
class SourceManager;
namespace syntax {
class ExpressionSyntax;
class FunctionPrototypeSyntax;
class PropertyExprSyntax;
class SyntaxTree;
}
}

/// Convert a SourceManager file name to a URI.  slang may already store a URI
/// for in-memory buffers; real file paths are normalised through uri_from_path().
std::string uri_from_file_name(std::string_view file_name);

/// Create a SourceManager configured the way every lazyverilog parse wants it.
///
/// slang otherwise runs weakly_canonical() on each candidate path it probes
/// while resolving an `include, and a miss walks the whole directory prefix
/// with canonical().  With N include directories that is N canonicalisations
/// per include per parsed file, which on a shared/HPC filesystem turns project
/// indexing into a metadata-call storm.  Disabling proximate paths keeps the
/// probe down to the failed open itself; buffer paths stay absolute and are
/// canonicalised once by uri_from_source_buffer().
std::unique_ptr<slang::SourceManager> make_lsp_source_manager();

/// Convert a SourceManager buffer to a URI using its absolute full path,
/// returning an empty string for invalid or non-file-backed buffers.
std::string uri_from_source_buffer(const slang::SourceManager& sm, slang::BufferID buffer);

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
/// macro expansion, while every one of those buffers expands inside the same
/// file buffer.  Keying only on the raw buffer therefore misses on almost every
/// token in macro-heavy code and re-canonicalizes a path that always resolves
/// to the same file.  The expanded-buffer level absorbs those misses; the raw
/// buffer level stays in front of it because walking the expansion chain takes
/// a SourceManager lock.
///
/// The resolver also carries the build's *scope*, because deciding which file a
/// token belongs to and deciding whether this build wants that file are the same
/// question asked twice.  An include-heavy design otherwise indexes the same
/// header once per including file: 60 modules sharing two large headers built
/// the same declarations and the same reference occurrences 60 times, which
/// measured 7.9 s and 1.8 GB where the unique work is ~2.0 s.  Restricting a
/// build to one file lets the analyzer index each header once and reference that
/// shard from every dependent.
class SourceFileIdResolver {
public:
    SourceFileID for_token(SyntaxIndex& index, const slang::SourceManager& sm,
                           const slang::parsing::Token& token);
    SourceFileID for_location(SyntaxIndex& index, const slang::SourceManager& sm,
                              slang::SourceLocation location);

    /// Restrict this build to entries originating in @p uri.  An empty URI (the
    /// default) keeps the historical behaviour of indexing every buffer the
    /// SyntaxTree covers, which is what live current-file builds want.
    void restrict_to_uri(std::string uri) { only_uri_ = std::move(uri); }

    /// Whether an entry at @p location belongs to this build's scope.
    ///
    /// Comparison is on resolved SourceFileID rather than raw BufferID: slang
    /// allocates a fresh buffer per macro expansion, so a raw-buffer test would
    /// reject every macro-expanded token in the very file being indexed.
    bool accepts(SyntaxIndex& index, const slang::SourceManager& sm,
                 slang::SourceLocation location);
    bool accepts(SyntaxIndex& index, const slang::SourceManager& sm,
                 const slang::parsing::Token& token);

    /// Restrict which out-of-scope declarations this build records.
    ///
    /// The set is fetched through @p provider on the first declaration that
    /// actually comes from another file, because scanning the scoped file costs
    /// more than it saves for a file that `include`s nothing worth dropping.
    /// The provider and whatever it returns must outlive the resolver; leaving
    /// it unset (the default) records every declaration.  See
    /// collect_mentioned_names().
    void set_mentions_provider(
        std::function<const std::unordered_set<std::string_view>*()> provider) {
        mentions_provider_ = std::move(provider);
    }

    /// Whether a declaration of @p name_token belongs in this build's shard.
    ///
    /// Declarations in scope always do.  One from an `include`d header is kept
    /// only when the scoped file mentions its name, because the sole reason a
    /// scoped build carries another file's declarations is to resolve its own
    /// tokens against them — the header's own shard is what records them for
    /// everyone else.  A header shared by N files otherwise costs N copies of
    /// its whole declaration set, rendered type text and all.
    bool wants_declaration(SyntaxIndex& index, const slang::SourceManager& sm,
                           const slang::parsing::Token& name_token);

private:
    std::unordered_map<uint32_t, SourceFileID> by_buffer_;
    std::unordered_map<uint32_t, SourceFileID> by_expanded_buffer_;
    std::string only_uri_;
    SourceFileID only_file_id_{kInvalidSourceFileID};
    std::function<const std::unordered_set<std::string_view>*()> mentions_provider_;
    const std::unordered_set<std::string_view>* mentioned_names_{nullptr};
};

/// Safely return a token's user-facing value text, or an empty string for a
/// missing token.  Keeping this helper shared prevents each indexer / feature
/// from making slightly different missing-token choices.
std::string token_value_text(const slang::parsing::Token& token);

bool syntax_fragment_edge_is_wordlike(char c);

/// Return where @p token was actually written, redirecting a macro-body
/// literal (e.g. the method name in a UVM-style `` `define FOO(...) task
/// get_next_item(...); ... endtask ``) to its true origin instead of the
/// macro-expansion buffer, which has no real line/column to report.  A plain
/// token, or a substituted macro argument, is returned unchanged.
slang::SourceLocation token_true_origin_location(const slang::SourceManager& sm,
                                                 const slang::parsing::Token& token);

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

/// Render the hover/documentation signature block for a function or task
/// prototype.  Shared so the closed-file shard and the open-buffer dynamic
/// index produce byte-identical `ValueEntry::signature` text for the same
/// declaration.
std::string make_subroutine_signature(const slang::syntax::FunctionPrototypeSyntax& proto,
                                      const std::string& name, const slang::SourceManager& sm);

/// Reduce a verbatim `extends` clause to the bare class name used as the
/// `ClassEntry` key.  The clause is stored as written, so it may carry a
/// package qualifier and parameter overrides (`extends cfg_pkg::base_cfg #(8)`)
/// that no index is keyed by.
std::string base_class_lookup_name(std::string_view base_class);

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

/// Identifier-shaped words a build scoped to a single file could possibly
/// resolve.
///
/// A scoped build records occurrences only for tokens in its own file, and
/// every lookup it makes keys on the text of such a token.  An `include`d
/// header contributes its declarations to every file that includes it, so one
/// widely shared header makes the occurrence lookup tables tens of thousands of
/// entries, almost none of which any token in the scoped file mentions.  Having
/// the scoped file's words up front lets those entries be skipped before they
/// are built.
///
/// The result is deliberately a superset.  It is scanned out of raw text, so
/// keywords and words inside comments and string literals are included too;
/// that only makes the filter less aggressive, never wrong.  Macro bodies are
/// added because a macro defined in a header can expand inside the scoped file,
/// and the tokens it expands to spell names that never appear in that file's
/// own text.
///
/// Returned views borrow from @p source and from @p tree, so the set must not
/// outlive either.
std::unordered_set<std::string_view> collect_mentioned_names(
    std::string_view source, const slang::syntax::SyntaxTree& tree);

/// Combined single-pass replacement for collect_reference_occurrences() and
/// collect_macro_reference_occurrences().  Performs one SyntaxTree traversal
/// instead of two by composing the macro-expansion visitToken() and the
/// reference-resolution visitToken() into a single CombinedVisitor.
/// The SubroutineDeclarationCollector pre-pass still runs as a separate walk
/// because its output (declared_subroutines) is consumed by the main visitor.
/// @param restrict_to_uri  when non-empty, record only occurrences originating
///        in that file, so an `include`d header's occurrences are collected once
///        into the header's own shard instead of once per including file.
/// @param mentioned_names  when non-null, the words the scoped file can mention
///        (see collect_mentioned_names).  Declarations whose name is absent are
///        left out of the lookup tables, which no in-scope token could have
///        matched anyway.
void collect_combined_occurrences(
    const slang::syntax::SyntaxTree& tree, const slang::syntax::SyntaxNode& root,
    SyntaxIndex& index, const slang::SourceManager& sm, std::string_view restrict_to_uri = {},
    const std::unordered_set<std::string_view>* mentioned_names = nullptr);
