#pragma once
#include "document_state.hpp"

#include <slang/diagnostics/Diagnostics.h>
#include <slang/text/SourceManager.h>

#include <memory>
#include <vector>

/// What slang_fixes() learned from walking a diagnostic's scope, kept so each
/// scope is walked once per compile however many warnings it has.  Create one
/// per Compilation (or SyntaxTree) and drop it before that: it points into it.
class SlangFixContext {
public:
    SlangFixContext();
    ~SlangFixContext();
    SlangFixContext(const SlangFixContext&) = delete;
    SlangFixContext& operator=(const SlangFixContext&) = delete;

    struct Impl;
    std::unique_ptr<Impl> impl;
};

/// Fixes for a slang diagnostic: casts for the conversion warnings (width,
/// sign, type, float, boolean).  Empty when none applies.  Must run while the
/// diagnostic's Compilation is alive: it reads the types and symbol it carries.
std::vector<FixInfo> slang_fixes(const slang::SourceManager& sm, const slang::Diagnostic& diag,
                                 SlangFixContext& context);
