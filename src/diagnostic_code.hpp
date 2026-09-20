#pragma once

#include <slang/diagnostics/DiagnosticEngine.h>
#include <slang/diagnostics/Diagnostics.h>

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
inline std::string slang_diagnostic_code(const slang::DiagnosticEngine& engine,
                                         slang::DiagCode code) {
    const std::string_view option = engine.getOptionName(code);
    if (!option.empty())
        return std::string(option);
    return std::string(slang::toString(code));
}
