#pragma once

#include <slang/diagnostics/DiagnosticEngine.h>
#include <slang/diagnostics/Diagnostics.h>
#include <slang/diagnostics/LexerDiags.h>
#include <slang/driver/CompatSettings.h>
#include <slang/text/SourceManager.h>

#include <string>

/// The stable, user-facing name for a slang diagnostic -- what `--nowarn` takes
/// and what the CLI prints in brackets at the end of each line.
///
/// slang gives its 169 warnings a `-W` option name (`width-trunc`,
/// `implicit-conv`, ...) and gives its ~970 errors none at all, because an error
/// is not something you turn off with `-W`.  Reporting only the option name
/// would therefore leave every error un-nameable -- including `MissingTimeScale`,
/// which is the one `docs/linter/cli.md` already documents as the diagnostic most
/// likely to bury a run.
///
/// So the name falls back to the diagnostic's own enum spelling
/// (`toString(DiagCode)` -> "MissingTimeScale").  The two families never collide:
/// slang's option names are lowercase-with-hyphens and its enum names are
/// CamelCase, which is also how the CLI tells them apart when validating a
/// `--nowarn` value.
/// The severity slang's own driver reports for @p code.  Since v11 many errors
/// are warnings that the driver promotes back to errors by default
/// (`CompatSettings`, e.g. `redefinition`); getDefaultSeverity() alone would
/// report them as warnings.
inline slang::DiagnosticSeverity slang_diagnostic_severity(slang::DiagCode code) {
    static const slang::DiagnosticEngine& engine = []() -> const slang::DiagnosticEngine& {
        static slang::SourceManager sm;
        static slang::DiagnosticEngine configured(sm);
        slang::driver::CompatSettings{}.configureDiagnostics(configured);
        return configured;
    }();
    return engine.getSeverity(code, slang::SourceLocation::NoLocation);
}

/// Diagnostics no lazyverilog binary reports.  `newline-eof` (slang v12) is a
/// style warning slang's own driver leaves off by default, and it fires on
/// most real files.
inline bool slang_diagnostic_hidden(slang::DiagCode code) {
    return code == slang::diag::NewlineEOF;
}

inline std::string slang_diagnostic_code(const slang::DiagnosticEngine& engine,
                                         slang::DiagCode code) {
    const std::string_view option = engine.getOptionName(code);
    if (!option.empty())
        return std::string(option);
    return std::string(slang::toString(code));
}
