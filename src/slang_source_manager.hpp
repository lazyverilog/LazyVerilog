#pragma once

#include <memory>
#include <string_view>

#include <slang/diagnostics/Diagnostics.h>
#include <slang/parsing/Lexer.h>
#include <slang/text/SourceManager.h>
#include <slang/util/BumpAllocator.h>

/// Every `slang::SourceManager` this server builds goes through here.
///
/// slang otherwise computes a "proximate" (working-directory-relative) display
/// name for each buffer it caches, and that runs `weakly_canonical()` over the
/// working directory: one `readlink` per path component, every time a buffer is
/// assigned.  A parse pays it per file; a bare lex pays it per request.
///
/// On the edit path that is the difference between free and a metadata walk per
/// keystroke -- Neovim re-requests folds from every `didChange` -- and on the
/// shared/HPC filesystems this server targets each component is a network round
/// trip.  Buffer paths are kept absolute and are canonicalised once by
/// `uri_from_source_buffer()`, so nothing downstream needs the proximate name.
///
/// This lives in its own header, rather than beside the index helpers that first
/// needed it, so the formatter and folding lexers can share the one definition
/// without pulling `syntax_index.hpp` into the formatter layer.
inline void configure_lsp_source_manager(slang::SourceManager& source_manager) {
    source_manager.setDisableProximatePaths(true);
}

/// Allocate a SourceManager configured the way every lazyverilog parse wants it.
inline std::unique_ptr<slang::SourceManager> make_lsp_source_manager() {
    auto source_manager = std::make_unique<slang::SourceManager>();
    configure_lsp_source_manager(*source_manager);
    return source_manager;
}

/// A `slang::parsing::Lexer` over one in-memory buffer, with the manager,
/// allocator and diagnostic sink it borrows kept alive alongside it.
///
/// The four pieces are bundled rather than spelled out at each call site because
/// three of them are pure scaffolding and the fourth -- the SourceManager --
/// has to be configured before `assignText()` runs or the lex costs a
/// canonicalisation of the working directory.  Both bare-lex callers had
/// default-constructed one and paid exactly that, on the two requests an editor
/// issues most: `textDocument/foldingRange` and `textDocument/formatting`.
/// Owning the scaffolding here is what makes forgetting impossible rather than
/// merely unlikely.
class SourceTextLexer {
  public:
    explicit SourceTextLexer(std::string_view text) {
        configure_lsp_source_manager(source_manager_);
        auto buffer = source_manager_.assignText(text);
        lexer_ = std::make_unique<slang::parsing::Lexer>(buffer, allocator_, diagnostics_,
                                                         source_manager_);
    }

    SourceTextLexer(const SourceTextLexer&) = delete;
    SourceTextLexer& operator=(const SourceTextLexer&) = delete;

    slang::parsing::Token lex() { return lexer_->lex(); }

  private:
    slang::SourceManager source_manager_;
    slang::BumpAllocator allocator_;
    slang::Diagnostics diagnostics_;
    // By pointer: Lexer holds references to the three members above, so it must
    // be constructed after them and must not be moved.
    std::unique_ptr<slang::parsing::Lexer> lexer_;
};
