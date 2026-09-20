#include "lint_codes.hpp"

#include <algorithm>
#include <array>

namespace {

// Ordered the way `--help` should read it: by config section, and within a
// section the way options.md introduces the rules.
constexpr std::array kLintCodes{
    LintCodeInfo{LintCode::NamingModule, "lint-naming-module",
                 "module name does not match its pattern"},
    LintCodeInfo{LintCode::NamingInterface, "lint-naming-interface",
                 "interface name does not match its pattern"},
    LintCodeInfo{LintCode::NamingInputPort, "lint-naming-input-port",
                 "input port name does not match its pattern"},
    LintCodeInfo{LintCode::NamingOutputPort, "lint-naming-output-port",
                 "output port name does not match its pattern"},
    LintCodeInfo{LintCode::NamingSignal, "lint-naming-signal",
                 "signal name does not match its pattern"},
    LintCodeInfo{LintCode::NamingStruct, "lint-naming-struct",
                 "struct name does not match its pattern"},
    LintCodeInfo{LintCode::NamingUnion, "lint-naming-union",
                 "union name does not match its pattern"},
    LintCodeInfo{LintCode::NamingEnum, "lint-naming-enum",
                 "enum name does not match its pattern"},
    LintCodeInfo{LintCode::NamingParameter, "lint-naming-parameter",
                 "parameter name does not match its pattern"},
    LintCodeInfo{LintCode::NamingLocalparam, "lint-naming-localparam",
                 "localparam name does not match its pattern"},
    LintCodeInfo{LintCode::NamingRegister, "lint-naming-register",
                 "always_ff register name does not match its pattern"},
    LintCodeInfo{LintCode::NamingModuleFilename, "lint-naming-module-filename",
                 "module name differs from the file it is declared in"},
    LintCodeInfo{LintCode::NamingPackageFilename, "lint-naming-package-filename",
                 "package name differs from the file it is declared in"},

    LintCodeInfo{LintCode::ModuleOnePerFile, "lint-module-one-per-file",
                 "more than one module declared in one file"},

    LintCodeInfo{LintCode::InstanceStyle, "lint-instance-style",
                 "instance port connections do not follow the required style"},
    LintCodeInfo{LintCode::InstanceDuplicateConnection, "lint-instance-duplicate-connection",
                 "the same port is connected twice in one instance"},
    LintCodeInfo{LintCode::InstanceStaleConnection, "lint-instance-stale-connection",
                 "instance connects a port the module does not declare"},
    LintCodeInfo{LintCode::InstanceMissingConnection, "lint-instance-missing-connection",
                 "instance omits a port the module declares"},

    LintCodeInfo{LintCode::StatementCaseMissingDefault, "lint-statement-case-missing-default",
                 "case statement has no default item"},
    LintCodeInfo{LintCode::StatementRawAlways, "lint-statement-raw-always",
                 "raw always block instead of always_comb/always_ff/always_latch"},
    LintCodeInfo{LintCode::StatementLatchInference, "lint-statement-latch-inference",
                 "always_comb block may infer a latch"},
    LintCodeInfo{LintCode::StatementAssignmentKind, "lint-statement-assignment-kind",
                 "blocking/nonblocking assignment does not match the block kind"},
    LintCodeInfo{LintCode::StatementExplicitBegin, "lint-statement-explicit-begin",
                 "single-statement body without begin/end"},

    LintCodeInfo{LintCode::FunctionAutomatic, "lint-function-automatic",
                 "function declaration is not automatic"},
    LintCodeInfo{LintCode::FunctionCallStyle, "lint-function-call-style",
                 "call arguments do not follow the required style"},
    LintCodeInfo{LintCode::FunctionExplicitLifetime, "lint-function-explicit-lifetime",
                 "function declaration has no explicit lifetime"},
    LintCodeInfo{LintCode::TaskExplicitLifetime, "lint-task-explicit-lifetime",
                 "task declaration has no explicit lifetime"},

    LintCodeInfo{LintCode::StyleTrailingWhitespace, "lint-style-trailing-whitespace",
                 "line ends with spaces or tabs"},
};

static_assert(kLintCodes.size() == static_cast<size_t>(LintCode::Count),
              "every LintCode needs a row in kLintCodes: adding a rule without one "
              "would give it another rule's code");

/// Does @p code continue past @p prefix at a `-` boundary?
///
/// The boundary is the whole point: without it "lint-nam" would silence every
/// naming rule, which makes a typo look like it worked.
bool is_hierarchical_prefix(std::string_view prefix, std::string_view code) {
    return code.size() > prefix.size() && code.compare(0, prefix.size(), prefix) == 0 &&
           code[prefix.size()] == '-';
}

} // namespace

std::span<const LintCodeInfo> all_lint_codes() { return kLintCodes; }

const LintCodeInfo& lint_code_info(LintCode id) {
    for (const auto& entry : kLintCodes) {
        if (entry.id == id)
            return entry;
    }
    // Unreachable: the static_assert above pins the row count to the enumerator
    // count, and "lint codes are one-to-one with the table" pins the ids.  The
    // fallback only keeps this function total.
    return kLintCodes.front();
}

bool lint_code_matches(std::string_view nowarn, std::string_view code) {
    return nowarn == code || is_hierarchical_prefix(nowarn, code);
}

bool is_known_lint_nowarn(std::string_view nowarn) {
    return std::any_of(kLintCodes.begin(), kLintCodes.end(), [&](const LintCodeInfo& entry) {
        return lint_code_matches(nowarn, entry.code);
    });
}
