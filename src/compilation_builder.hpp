#pragma once

#include "analyzer.hpp"

#include <memory>
#include <slang/ast/Compilation.h>
#include <slang/text/SourceManager.h>
#include <slang/util/Bag.h>
#include <string>

/// Everything one elaborated design needs to stay alive together: the
/// SourceManager the syntax trees point into, the Bag they were parsed with,
/// and the Compilation itself.
///
/// Member order is load-bearing.  Members are destroyed in reverse declaration
/// order, so `compilation` (which holds references into both) must be declared
/// last.
struct BuiltCompilation {
    std::unique_ptr<slang::SourceManager> source_manager;
    std::unique_ptr<slang::Bag> bag;
    std::unique_ptr<slang::ast::Compilation> compilation;
    /// URI of the first file that produced a syntax tree, used as the fallback
    /// location for diagnostics that carry no usable source location.
    std::string first_uri;
};

/// Parse every file in `snapshot` into one Compilation configured with
/// `options`.
///
/// Callers differ in `options`: the background semantic-diagnostics path sets
/// `CompilationFlags::LintMode` so every module elaborates standalone, while
/// tools that need a real instance hierarchy must leave it off and let slang
/// pick actual top-level modules.
///
/// Files whose path was already pulled in by an earlier tree (via `include`)
/// are skipped, as are files that fail to parse.  When `log_skipped` is true,
/// each skipped file is reported on stderr.
BuiltCompilation build_compilation(const CompilationSnapshot& snapshot,
                                   const slang::ast::CompilationOptions& options,
                                   bool log_skipped);
