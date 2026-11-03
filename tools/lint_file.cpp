#include "banner.hpp"
#include "cli_project.hpp"
#include "document_state.hpp"
#include "fix_apply.hpp"
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
    std::cerr << "Usage: lazyverilog-lint [-f <filelist>] [--lint-only | --compile-only] "
                 "[--maxerror <n>] [--nowarn <code>]\n"
                 "       [--fix | --fix-unsafe | --fix-interactive] [--version] [--] [<file>]\n"
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
           "      --fix                  Apply the safe fixes in place and print what is left.\n"
           "      --fix-unsafe           Apply safe and unsafe fixes in place.\n"
           "      --fix-interactive      Show each fix as a diff and ask: y, n, a (all), q (quit).\n"
           "                             The three --fix options cannot be combined.\n"
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

enum class FixMode { None, Safe, Unsafe, Interactive };

void print_diag(std::ostream& os, const FlatDiag& item) {
    os << item.file << ":" << (item.diag.line + 1) << ":" << (item.diag.col + 1) << ": "
       << severity_text(item.diag.severity) << ": " << item.diag.message;
    if (!item.diag.code.empty())
        os << " [" << item.diag.code << "]";
    os << "\n";
}

std::string read_binary(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> lines;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos)
            nl = text.size();
        size_t end = nl;
        if (end > pos && text[end - 1] == '\r')
            --end;
        lines.push_back(text.substr(pos, end - pos));
        pos = nl + 1;
    }
    return lines;
}

/// Print the lines @p fix touches as they are now (-) and with only that fix applied (+).
void print_fix_preview(const std::string& text, const FixInfo& fix) {
    int first = std::numeric_limits<int>::max();
    int last = 0;
    for (const auto& e : fix.edits) {
        first = std::min(first, e.start_line);
        last = std::max(last, e.end_line);
    }
    const auto old_lines = split_lines(text);
    const auto new_lines = split_lines(apply_fixes(text, {&fix}).text);
    const int delta = static_cast<int>(new_lines.size()) - static_cast<int>(old_lines.size());
    for (int i = first; i <= last && i < static_cast<int>(old_lines.size()); ++i)
        std::cout << "-" << old_lines[i] << "\n";
    for (int i = first; i <= last + delta && i < static_cast<int>(new_lines.size()); ++i)
        std::cout << "+" << new_lines[i] << "\n";
}

/// Applies fixes to the files in @p flat (sorted by file) and marks in @p fixed the diagnostics
/// whose fix went in.
void apply_cli_fixes(FixMode mode, const std::vector<std::shared_ptr<const DocumentState>>& states,
                     const std::vector<FlatDiag>& flat, std::vector<bool>& fixed) {
    size_t total_applied = 0, total_skipped = 0, files_changed = 0;
    bool accept_all = false, quit = false;

    for (size_t begin = 0; begin < flat.size() && !quit;) {
        size_t end = begin;
        while (end < flat.size() && flat[end].file == flat[begin].file)
            ++end;
        const std::string& file = flat[begin].file;
        const size_t group_begin = begin;
        begin = end;

        const DocumentState* state = nullptr;
        for (const auto& st : states)
            if (st && path_from_file_uri(st->uri) == file)
                state = st.get();
        if (!state)
            continue;

        // (diagnostic index, fix) in priority (position) order; one fix per diagnostic.
        std::vector<std::pair<size_t, const FixInfo*>> candidates;
        for (size_t i = group_begin; i < end; ++i)
            for (const auto& fix : flat[i].diag.fixes)
                if (mode != FixMode::Safe || fix.safe) {
                    candidates.emplace_back(i, &fix);
                    break;
                }
        if (candidates.empty())
            continue;

        // Positions are only valid against the text the analyzer parsed.
        const std::string original = read_binary(file);
        if (original != state->text) {
            std::cerr << file << ": changed on disk; not fixed\n";
            continue;
        }

        if (mode == FixMode::Interactive) {
            std::vector<std::pair<size_t, const FixInfo*>> accepted;
            for (const auto& cand : candidates) {
                bool take = accept_all;
                if (!take && !quit) {
                    print_diag(std::cout, flat[cand.first]);
                    std::cout << "  fix: " << cand.second->title
                              << (cand.second->safe ? "" : " [unsafe]") << "\n";
                    print_fix_preview(original, *cand.second);
                    std::cout << "Apply this fix [y,n,a,q]? " << std::flush;
                    std::string answer;
                    if (!std::getline(std::cin, answer))
                        answer = "q"; // EOF
                    const char c = answer.empty() ? 'n'
                                                  : static_cast<char>(std::tolower(
                                                        static_cast<unsigned char>(answer[0])));
                    if (c == 'a')
                        accept_all = take = true;
                    else if (c == 'y')
                        take = true;
                    else if (c == 'q')
                        quit = true;
                }
                if (take)
                    accepted.push_back(cand);
            }
            candidates = std::move(accepted);
        }
        if (candidates.empty())
            continue;

        std::vector<const FixInfo*> fixes;
        for (const auto& cand : candidates)
            fixes.push_back(cand.second);
        const FixApplyResult result = apply_fixes(original, fixes);

        // Write beside the original and rename over it, so a failed write cannot truncate it.
        bool wrote = true;
        // shortcut: hard links and ownership are not preserved, upgrade to an in-place write if that matters
        if (result.text != original) {
            namespace fs = std::filesystem;
            std::error_code ec;
            const fs::path target = fs::canonical(file, ec); // follow symlinks
            const auto perms = ec ? fs::perms::none : fs::status(target, ec).permissions();
            if (ec || (perms & fs::perms::owner_write) == fs::perms::none) {
                wrote = false; // missing or read-only: leave it alone
            } else {
                const fs::path tmp = target.string() + ".lazyverilog-fix.tmp";
                {
                    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
                    out << result.text;
                    out.flush();
                    out.close();
                    wrote = static_cast<bool>(out);
                }
                if (wrote)
                    fs::permissions(tmp, perms, ec);
                if (wrote && !ec)
                    fs::rename(tmp, target, ec);
                if (!wrote || ec) {
                    wrote = false;
                    fs::remove(tmp, ec);
                }
            }
        }
        if (!wrote) {
            std::cerr << file << ": cannot write; not fixed\n";
            continue;
        }
        for (size_t k = 0; k < candidates.size(); ++k)
            fixed[candidates[k].first] = result.outcomes[k] != FixOutcome::Skipped;

        total_applied += result.applied;
        total_skipped += result.skipped;
        if (result.text != original)
            ++files_changed;
    }

    std::cerr << "fixed " << total_applied << " issue(s) in " << files_changed << " file(s)\n";
    if (total_skipped != 0)
        std::cerr << "skipped " << total_skipped << " overlapping fix(es); run again\n";
}

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
    FixMode fix_mode = FixMode::None;
    int fix_flags = 0;

    bool end_of_options = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--" && !end_of_options) {
            end_of_options = true;
            continue;
        }
        if (end_of_options || arg.empty() || arg[0] != '-') {
            if (!file_arg.empty()) {
                std::cerr << "Only one file can be linted per run (use -f for more): " << arg << "\n";
                print_usage();
                return 1;
            }
            file_arg = arg;
            continue;
        }
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
        } else if (arg == "--fix" || arg == "--fix-unsafe" || arg == "--fix-interactive") {
            ++fix_flags;
            fix_mode = arg == "--fix"          ? FixMode::Safe
                       : arg == "--fix-unsafe" ? FixMode::Unsafe
                                               : FixMode::Interactive;
        } else if (arg == "--version") {
            std::cout << "lazyverilog-lint " << LAZYVERILOG_VERSION << "\n";
            return 0;
        } else if (arg == "-h" || arg == "--help") {
            print_help(nowarn_engine);
            return 0;
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            print_usage();
            return 1;
        }
    }

    if (filelist_arg.empty() && file_arg.empty()) {
        print_usage();
        return 1;
    }
    if (lint_only && compile_only) {
        // Together they would skip both halves and report a clean run.
        std::cerr << "--lint-only and --compile-only cannot be used together\n";
        print_usage();
        return 1;
    }

    if (fix_flags > 1) {
        std::cerr << "--fix, --fix-unsafe and --fix-interactive cannot be used together\n";
        print_usage();
        return 1;
    }

    std::string file_text;
    if (!file_arg.empty()) {
        std::ifstream f(file_arg, std::ios::binary); // byte-exact: fixes apply by position
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
    if (!compile_only && project.config.lint.instance.enable &&
        project.config.lint.instance.stale_instance_diagnostic)
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

    std::stable_sort(flat.begin(), flat.end(), [](const FlatDiag& a, const FlatDiag& b) {
        if (a.file != b.file)
            return a.file < b.file;
        if (a.diag.line != b.diag.line)
            return a.diag.line < b.diag.line;
        return a.diag.col < b.diag.col;
    });

    std::vector<bool> fixed(flat.size(), false);
    if (fix_mode != FixMode::None)
        apply_cli_fixes(fix_mode, states, flat, fixed);

    bool has_error = false;
    for (size_t i = 0; i < flat.size(); ++i) {
        if (fixed[i])
            continue;
        const auto& item = flat[i];
        print_diag(std::cout, item);
        if (item.diag.severity == 1)
            has_error = true;
    }

    return has_error ? 2 : 0;
}
