#pragma once

#include <cstddef>
#include <span>
#include <string_view>

/// Identifier for one lazyverilog lint rule.
///
/// This enum -- not a string at the emit site -- is what a rule reports itself
/// as.  The code text and the `--help` description live in one table keyed on
/// it (`lint_code_info()`), so a rule cannot be given a code that `--nowarn`
/// does not know about, and a code cannot be listed in `--help` that no rule
/// emits.  Both halves of that used to be possible when the code was a string
/// literal typed at each of the thirty-odd push_diag() calls.
///
/// The codes are hierarchical (`lint-naming-module`), and `--nowarn` matches on
/// `-` boundaries, so `lint-naming` silences every naming rule and `lint`
/// silences the linter entirely.  slang's own diagnostics keep slang's names and
/// never start with `lint`, which is what keeps the two namespaces apart.
enum class LintCode {
    // [lint.naming]
    NamingModule,
    NamingInterface,
    NamingInputPort,
    NamingOutputPort,
    NamingSignal,
    NamingStruct,
    NamingUnion,
    NamingEnum,
    NamingParameter,
    NamingLocalparam,
    NamingRegister,
    NamingModuleFilename,
    NamingPackageFilename,

    // [lint.module]
    ModuleOnePerFile,

    // [lint.instance]
    InstanceStyle,
    InstanceDuplicateConnection,
    InstanceStaleConnection,
    InstanceMissingConnection,

    // [lint.statement]
    StatementCaseMissingDefault,
    StatementRawAlways,
    StatementLatchInference,
    StatementAssignmentKind,
    StatementExplicitBegin,

    // [lint.function]
    FunctionAutomatic,
    FunctionCallStyle,
    FunctionExplicitLifetime,
    TaskExplicitLifetime,

    // [lint.style]
    StyleTrailingWhitespace,

    /// Not a rule.  It is what makes "every enumerator has a table entry" a
    /// static_assert in lint_codes.cpp rather than a lookup that silently
    /// returns the wrong code at request time.  Keep it last.
    Count,
};

struct LintCodeInfo {
    LintCode id;
    /// The `--nowarn` spelling, e.g. "lint-naming-module".
    std::string_view code;
    /// One line for `--help`, phrased as what the rule flags.
    std::string_view description;
};

/// The table, in `--help` order.
std::span<const LintCodeInfo> all_lint_codes();

/// The entry for @p id.  Every enumerator has one; a missing entry is a build
/// error rather than an empty code at request time.
const LintCodeInfo& lint_code_info(LintCode id);

/// True when @p nowarn silences @p code, matching on `-` boundaries so that
/// "lint-naming" covers "lint-naming-module" but "lint-nam" covers nothing.
bool lint_code_matches(std::string_view nowarn, std::string_view code);

/// True when @p nowarn names a lint code or a prefix of one, i.e. when it would
/// silence at least one rule.  A `--nowarn` value that starts with "lint" and
/// fails this is a typo, and the CLI says so instead of quietly matching
/// nothing.
bool is_known_lint_nowarn(std::string_view nowarn);
