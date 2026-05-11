#pragma once
#include <memory>
#include <optional>
#include <string>
#include <vector>

// Forward declarations from slang
namespace slang::syntax { class SyntaxTree; }
namespace slang::ast { class Compilation; }

/// Pre-formatted diagnostic info extracted at parse time.
/// Avoids copying slang::Diagnostic (whose ConstantValue args are
/// not safely copyable — internal arena pointers become dangling).
struct ParseDiagInfo {
    int    line{0};   // 0-based
    int    col{0};    // 0-based
    int    severity{3}; // lsDiagnosticSeverity: 1=Error,2=Warn,3=Info,4=Hint
    std::string message;
};

/// Immutable snapshot of a single open document.
/// Handlers receive a shared_ptr<const DocumentState>; didChange atomically
/// swaps in a new instance. No per-document locking needed on the read path.
struct DocumentState {
    std::string uri;
    std::string text;
    std::shared_ptr<slang::syntax::SyntaxTree> tree;
    // Pre-formatted diagnostics extracted in make_state() while the
    // SyntaxTree and its arena allocators are still alive.
    std::vector<ParseDiagInfo> parse_diagnostics;
    // Compilation is optional — only present when background_compilation=true
    std::optional<std::shared_ptr<slang::ast::Compilation>> compilation;
    std::string tree_filename{"buffer.sv"};
    bool compilation_dirty{false};

    DocumentState() = default;
    DocumentState(std::string uri, std::string text,
                  std::shared_ptr<slang::syntax::SyntaxTree> tree)
        : uri(std::move(uri)), text(std::move(text)), tree(std::move(tree)) {}
};
