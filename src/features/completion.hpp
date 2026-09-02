#pragma once
#include "../analyzer.hpp"
#include "../document_state.hpp"
#include "LibLsp/lsp/lsTextDocumentPositionParams.h"
#include "LibLsp/lsp/textDocument/completion.h"
#include <atomic>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

// ── Completion context ────────────────────────────────────────────────────────

enum class CompletionContextKind {
    Identifier,   // general visible symbols + keywords
    MemberAccess, // foo.
    PackageScope, // pkg::
    NamedPort,    // .portname inside module instantiation ( )
    Parameter,    // .param inside #( )
    Macro,        // `
    IncludeFile,  // `include "
    NewExpression, // after new
    BaseClass,    // after extends / implements
    EventControl, // inside @( ... )
    Unknown,      // fallback — treated as Identifier
};

enum class KeywordContextKind {
    General,
    ModuleItem,
    Procedural,
    Class,
    Covergroup,
};

struct CompletionContext {
    CompletionContextKind kind{CompletionContextKind::Unknown};
    std::string prefix;     // text already typed (for prefix/fuzzy matching)
    std::string scope_name; // LHS of . or :: (MemberAccess / PackageScope)
    // Segment left of scope_name in a `a::b::` chain, empty when there is none.
    // `my_item::type_id::` yields scope_name "type_id", scope_qualifier
    // "my_item" — and only the qualifier says which class's `type_id` is meant.
    std::string scope_qualifier;
    // Field hops between scope_name and the cursor in a `a.b.c.` member access,
    // empty for `a.`.  scope_name names a declaration; each hop is a field of
    // the type resolved so far, so they resolve through different lookups.
    std::vector<std::string> member_chain;
    std::string current_scope_name; // best-effort enclosing module/interface/package
    // Type of the member-access receiver when the receiver is an implicit
    // handle -- `this` (the enclosing class) or `super` (its base) -- rather
    // than a named value.  There is no declaration to look up in that case, so
    // this is filled in during context detection and short-circuits the
    // variable-to-type resolution.  Empty for an ordinary `handle.` receiver.
    std::string receiver_type;
    std::string expected_type; // best-effort RHS expected type, e.g. enum typedef on assignment
    KeywordContextKind keyword_context{KeywordContextKind::General};
    int line{0};            // 0-based cursor line
    int col{0};             // 0-based cursor column
    // Port/parameter names already wired in the current instantiation (NamedPort / Parameter)
    std::unordered_set<std::string> connected_ports;
    // Macro names visible from the current document's preprocessing context.
    // Extra-file macros are intentionally not flattened into unrelated files;
    // they appear here only when the current file itself defines/includes them.
    std::unordered_set<std::string> visible_macros;
    // Package imports visible from the current document only.  Extra-file
    // imports must not leak into unrelated buffers; otherwise one demo/test
    // file with `import uvm_pkg::*` makes UVM package members appear
    // everywhere in the merged design index.
    std::vector<ImportEntry> visible_imports;
    // .svh/.vh paths from the design filelist (populated for IncludeFile context)
    std::vector<std::string> header_files;
};

// ── Cancellation ─────────────────────────────────────────────────────────────

struct CancellationToken {
    std::atomic<bool> cancelled{false};
    CancellationToken() = default;
    CancellationToken(const CancellationToken&) = delete;
    CancellationToken& operator=(const CancellationToken&) = delete;
};

struct CompletionCancelled {};

// ── Provider interface ────────────────────────────────────────────────────────

class CompletionProvider {
  public:
    virtual ~CompletionProvider() = default;

    /// Return true if this provider contributes items for ctx.
    virtual bool accepts(const CompletionContext& ctx) const = 0;

    /// Return completion items. Must be thread-safe (providers are stateless).
    virtual std::vector<lsCompletionItem> provide(const CompletionContext& ctx,
                                                   const SyntaxIndex& index,
                                                   const CancellationToken& tok) const = 0;
};

// ── Engine ────────────────────────────────────────────────────────────────────

class CompletionEngine {
  public:
    CompletionEngine();

    /// Build the completion list for the given cursor position.
    CompletionList complete(const lsTextDocumentPositionParams& params,
                            const DocumentState& state,
                            const Analyzer& analyzer,
                            const CancellationToken& tok) const;

  private:
    std::vector<std::unique_ptr<CompletionProvider>> providers_;

    CompletionContext detect_context(const DocumentState& state, int line, int col,
                                     const SyntaxIndex& index) const;

    void rank_and_sort(std::vector<lsCompletionItem>& items, const CompletionContext& ctx,
                       const std::unordered_set<std::string>& local_names,
                       const std::unordered_set<std::string>& expected_names,
                       const std::unordered_set<std::string>& same_type_names) const;
};
