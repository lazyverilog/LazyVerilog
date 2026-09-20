#include "banner.hpp"
#include "cli_project.hpp"
#include "document_state.hpp"
#include "features/lint.hpp"
#include "features/lint_codes.hpp"
#include "string_utils.hpp"

#include <slang/diagnostics/DiagnosticEngine.h>
#include <slang/text/SourceManager.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace {

void print_usage() {
    std::cerr << "Usage: lazyverilog-lint [-f <filelist>] [--lint-only] [--compile-only] "
                 "[--maxerror <n>] [--nowarn <code>] [--version] [<file>]\n"
                 "Run lazyverilog-lint --help for the list of diagnostic codes.\n";
}

/// slang's warning groups.  slang exposes no way to enumerate them -- the table
/// is `static` inside its generated DiagCode.cpp and only `findDiagGroup(name)`
/// is public -- so the names are listed here and then *probed* against slang
/// before being printed.  A group slang renames or drops therefore disappears
/// from --help instead of being advertised and then rejected.
constexpr std::string_view kSlangGroupNames[] = {"default",    "extra",  "pedantic",
                                                 "conversion", "unused", "parentheses"};

bool looks_like_slang_diag_name(std::string_view text) {
    // slang's -W option names are lowercase-with-hyphens; its diagnostic enum
    // names are CamelCase.  That is the whole distinction, and it is why a
    // lint code can never be mistaken for either.
    return !text.empty() && std::isupper(static_cast<unsigned char>(text.front())) != 0;
}

void print_help(const slang::DiagnosticEngine& engine) {
    std::cout
        << "Usage: lazyverilog-lint [options] [<file>]\n"
           "\n"
           "Lint and compile one SystemVerilog file, or every file in a filelist.\n"
           "At least one of -f <filelist> or <file> is required.\n"
           "\n"
           "Options:\n"
           "  -f, --filelist <filelist>  Project filelist (.f) to index.  With no <file>,\n"
           "                             lint every file it lists.  Overrides [design] vcode.\n"
           "      --lint-only            Print only lint-rule diagnostics.\n"
           "      --compile-only         Print only compilation diagnostics.\n"
           "      --maxerror <n>         Compilation error limit (default 64; 0 = unlimited).\n"
           "      --nowarn <code>        Do not report diagnostics with this code.  May be\n"
           "                             given more than once.  A suppressed error also\n"
           "                             stops contributing to the exit status.\n"
           "      --version              Print the version and exit.\n"
           "  -h, --help                 Print this help and exit.\n"
           "\n"
           "Diagnostic codes:\n"
           "  Every diagnostic is printed with its code in brackets, and that code is\n"
           "  exactly what --nowarn takes:\n"
           "\n"
           "      rtl/alu.sv:12:5: warning: implicit conversion from 'a' to 'b'"
           " [implicit-conv]\n"
           "      $ lazyverilog-lint --nowarn implicit-conv rtl/alu.sv\n"
           "\n"
           "  Lint-rule codes match on '-' boundaries, so --nowarn lint-naming silences\n"
           "  every naming rule and --nowarn lint silences the linter entirely:\n"
           "\n";

    size_t widest = 0;
    for (const auto& entry : all_lint_codes())
        widest = std::max(widest, entry.code.size());

    for (const auto& entry : all_lint_codes()) {
        std::cout << "      " << entry.code
                  << std::string(widest - entry.code.size() + 2, ' ') << entry.description
                  << "\n";
    }

    std::cout << "\n"
                 "  Each of those can also be turned off permanently in lazyverilog.toml --\n"
                 "  see docs/linter/options.md for the [lint] key behind each rule.\n"
                 "\n"
                 "  Compilation codes come from slang.  Warnings carry slang's own -W option\n"
                 "  name (width-trunc, implicit-conv, unused-net, ...), and slang's warning\n"
                 "  groups work as well:\n"
                 "\n"
                 "     ";
    for (const auto& name : kSlangGroupNames) {
        if (engine.findDiagGroup(name))
            std::cout << " " << name;
    }
    std::cout << "\n"
                 "\n"
                 "  slang errors have no -W name, so they are named by their slang diagnostic\n"
                 "  name instead (MissingTimeScale, UnknownModule, ...) -- again, exactly as\n"
                 "  printed in brackets.  slang offers no way to look those names up, so a\n"
                 "  CamelCase code is taken as written and a misspelled one silences\n"
                 "  nothing.  Every lowercase code is checked, and rejected if slang does\n"
                 "  not know it.\n";
}

/// Parse a `--maxerror` value.  Returns false for anything that is not a plain
/// non-negative decimal integer, so a typo'd flag value fails loudly instead of
/// silently becoming 0 ("unlimited") and quietly changing what gets reported.
bool parse_error_limit(const std::string& text, uint32_t& out) {
    if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos)
        return false;
    try {
        unsigned long long value = std::stoull(text);
        if (value > std::numeric_limits<uint32_t>::max())
            return false;
        out = static_cast<uint32_t>(value);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

/// The set of diagnostics `--nowarn` was told to drop.
///
/// Filtering happens here, on the finished diagnostic list, rather than through
/// slang's DiagnosticEngine::setWarningOptions().  The engine can only silence
/// slang *warnings*: it has nothing to say about lazyverilog's own lint rules,
/// and nothing to say about slang errors, which is where the need actually
/// starts (see the `--maxerror` section of docs/linter/cli.md).  One mechanism
/// covering all three is worth more than a second, narrower one that behaves
/// differently.  The cost is that a suppressed diagnostic is still produced, so
/// `--nowarn` does not buy back elaboration time and does not change what counts
/// against `--maxerror`.
class NowarnFilter {
public:
    /// Records @p code, or explains on stderr why it cannot be honored.
    bool add(const std::string& code, const slang::DiagnosticEngine& engine) {
        if (code.empty()) {
            std::cerr << "--nowarn requires a code\n";
            return false;
        }

        if (code.rfind("lint", 0) == 0) {
            if (!is_known_lint_nowarn(code)) {
                std::cerr << "Unknown --nowarn code: " << code
                          << " (no lint rule matches; see --help for the list)\n";
                return false;
            }
            lint_prefixes_.push_back(code);
            return true;
        }

        // A slang group stands for its members, and the members are what the
        // diagnostics are actually tagged with, so expand it once here rather
        // than consulting slang per diagnostic.
        if (const auto* group = engine.findDiagGroup(code)) {
            for (auto member : group->getDiags()) {
                const std::string_view option = engine.getOptionName(member);
                if (!option.empty())
                    exact_.insert(std::string(option));
            }
            return true;
        }

        if (!engine.findFromOptionName(code).empty()) {
            exact_.insert(code);
            return true;
        }

        if (looks_like_slang_diag_name(code)) {
            exact_.insert(code);
            return true;
        }

        std::cerr << "Unknown --nowarn code: " << code
                  << " (not a lint rule, a slang warning, or a slang warning group;"
                     " see --help)\n";
        return false;
    }

    bool empty() const { return exact_.empty() && lint_prefixes_.empty(); }

    bool suppressed(const std::string& code) const {
        if (code.empty())
            return false;
        if (exact_.count(code) != 0)
            return true;
        return std::any_of(lint_prefixes_.begin(), lint_prefixes_.end(),
                           [&](const std::string& prefix) {
                               return lint_code_matches(prefix, code);
                           });
    }

private:
    std::unordered_set<std::string> exact_;
    std::vector<std::string> lint_prefixes_;
};

std::string_view severity_text(int severity) {
    switch (severity) {
    case 1:
        return "error";
    case 2:
        return "warning";
    case 4:
        return "hint";
    default:
        return "info";
    }
}

struct FlatDiag {
    std::string file;
    ParseDiagInfo diag;
};

void collect(std::vector<FlatDiag>& out, const std::string& fallback_uri,
            std::vector<ParseDiagInfo> diags) {
    for (auto& d : diags) {
        const std::string target_uri = d.uri.empty() ? fallback_uri : d.uri;
        out.push_back(FlatDiag{path_from_file_uri(target_uri), std::move(d)});
    }
}

} // namespace

int main(int argc, char* argv[]) {
    print_startup_banner("lazyverilog-lint", argc, argv);

    // Validating a --nowarn code means asking slang whether it knows the name,
    // and that lookup lives on DiagnosticEngine.  It needs a SourceManager only
    // to report source locations, which this instance never does.
    slang::SourceManager nowarn_source_manager;
    slang::DiagnosticEngine nowarn_engine(nowarn_source_manager);

    std::string filelist_arg;
    std::string file_arg;
    bool lint_only = false;
    bool compile_only = false;
    uint32_t error_limit = kDefaultCompilationErrorLimit;
    NowarnFilter nowarn;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-f" || arg == "--filelist") {
            if (i + 1 >= argc) {
                print_usage();
                return 1;
            }
            filelist_arg = argv[++i];
        } else if (arg == "--maxerror") {
            if (i + 1 >= argc) {
                std::cerr << arg << " requires a value\n";
                print_usage();
                return 1;
            }
            const std::string value = argv[++i];
            if (!parse_error_limit(value, error_limit)) {
                std::cerr << "Invalid " << arg << " value: " << value
                          << " (expected a non-negative integer; 0 means unlimited)\n";
                return 1;
            }
        } else if (arg == "--nowarn") {
            if (i + 1 >= argc) {
                std::cerr << arg << " requires a code\n";
                print_usage();
                return 1;
            }
            if (!nowarn.add(argv[++i], nowarn_engine))
                return 1;
        } else if (arg == "--lint-only") {
            lint_only = true;
        } else if (arg == "--compile-only") {
            compile_only = true;
        } else if (arg == "--version") {
            std::cout << "lazyverilog-lint " << LAZYVERILOG_VERSION << "\n";
            return 0;
        } else if (arg == "-h" || arg == "--help") {
            print_help(nowarn_engine);
            return 0;
        } else if (!arg.empty() && arg[0] == '-') {
            std::cerr << "Unknown option: " << arg << "\n";
            print_usage();
            return 1;
        } else {
            file_arg = arg;
        }
    }

    if (filelist_arg.empty() && file_arg.empty()) {
        print_usage();
        return 1;
    }

    std::string file_text;
    if (!file_arg.empty()) {
        std::ifstream f(file_arg);
        if (!f) {
            std::cerr << "Cannot open " << file_arg << "\n";
            return 1;
        }
        std::ostringstream ss;
        ss << f.rdbuf();
        file_text = ss.str();
    }

    const std::filesystem::path start = file_arg.empty()
                                            ? std::filesystem::current_path()
                                            : std::filesystem::absolute(file_arg).parent_path();
    CliProject project = resolve_cli_project(start, filelist_arg);

    Analyzer analyzer;
    index_cli_project(analyzer, project);

    std::string target_uri;
    if (!file_arg.empty()) {
        target_uri = uri_from_path(std::filesystem::absolute(file_arg));
        analyzer.open(target_uri, file_text);
    }

    analyzer.wait_for_background_index_idle();
    if (!lint_only)
        run_synchronous_semantic_compile(analyzer, error_limit);

    std::shared_ptr<const ProjectIndexSnapshot> project_lint_index;
    if (!compile_only && project.config.lint.instance.stale_instance_diagnostic)
        project_lint_index = analyzer.project_index_snapshot();

    std::vector<std::shared_ptr<const DocumentState>> states;
    if (!file_arg.empty()) {
        if (auto state = analyzer.get_state(target_uri))
            states.push_back(std::move(state));
    } else {
        states = analyzer.project_file_states_sync();
    }

    std::vector<FlatDiag> flat;
    for (const auto& state : states) {
        if (!state)
            continue;
        if (!lint_only) {
            collect(flat, state->uri, state->parse_diagnostics);
            collect(flat, state->uri, analyzer.semantic_diagnostics(state->uri));
        }
        if (!compile_only)
            collect(flat, state->uri,
                    run_lint(*state, project.config.lint, project_lint_index.get()));
    }

    // Drop the suppressed ones before anything else looks at the list, so a
    // silenced error cannot set the exit status either.  That is the point of
    // silencing it.
    if (!nowarn.empty()) {
        flat.erase(std::remove_if(flat.begin(), flat.end(),
                                  [&](const FlatDiag& item) {
                                      return nowarn.suppressed(item.diag.code);
                                  }),
                   flat.end());
    }

    std::sort(flat.begin(), flat.end(), [](const FlatDiag& a, const FlatDiag& b) {
        if (a.file != b.file)
            return a.file < b.file;
        if (a.diag.line != b.diag.line)
            return a.diag.line < b.diag.line;
        return a.diag.col < b.diag.col;
    });

    bool has_error = false;
    for (const auto& item : flat) {
        std::cout << item.file << ":" << (item.diag.line + 1) << ":" << (item.diag.col + 1) << ": "
                  << severity_text(item.diag.severity) << ": " << item.diag.message;
        if (!item.diag.code.empty())
            std::cout << " [" << item.diag.code << "]";
        std::cout << "\n";
        if (item.diag.severity == 1)
            has_error = true;
    }

    return has_error ? 2 : 0;
}
