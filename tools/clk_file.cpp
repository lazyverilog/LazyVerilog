#include "cli_project.hpp"
#include "compilation_builder.hpp"
#include "features/clock_domain.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <slang/ast/ASTVisitor.h>
#include <slang/ast/Compilation.h>
#include <slang/ast/symbols/InstanceSymbols.h>
#include <slang/ast/symbols/PortSymbols.h>
#include <slang/diagnostics/DiagnosticEngine.h>
#include <slang/text/SourceManager.h>
#include <string>
#include <string_view>
#include <vector>

namespace {

void print_usage() {
    std::cerr << "Usage: lazyverilog-clk -f <filelist> <instance>\n"
              << "\n"
              << "  <instance>  hierarchical path (top.u_core.u_fifo), or a module\n"
              << "              type name when it resolves to exactly one instance.\n";
}

struct FoundInstance {
    const slang::ast::InstanceSymbol* symbol = nullptr;
    std::string path;
};

/// Walks the elaborated hierarchy and records every instance with its full
/// hierarchical path.  Statements and expressions are not visited: this pass
/// only needs the instance tree.
struct InstanceCollector : slang::ast::ASTVisitor<InstanceCollector, false, false> {
    std::vector<FoundInstance> instances;

    void handle(const slang::ast::InstanceSymbol& symbol) {
        instances.push_back(FoundInstance{&symbol, symbol.getHierarchicalPath()});
        visitDefault(symbol);
    }
};

/// Accepts a full hierarchical path, a trailing path suffix, a bare instance
/// name, or a module type name.  Returns every instance that matches, so the
/// caller can report an ambiguous name instead of silently picking one.
std::vector<FoundInstance> match_instances(const std::vector<FoundInstance>& all,
                                           const std::string& query) {
    std::vector<FoundInstance> matches;
    for (const auto& candidate : all) {
        const bool path_match =
            candidate.path == query ||
            (candidate.path.size() > query.size() &&
             candidate.path.compare(candidate.path.size() - query.size(), query.size(), query) ==
                 0 &&
             candidate.path[candidate.path.size() - query.size() - 1] == '.');
        const bool name_match = candidate.symbol->name == query ||
                                candidate.symbol->getDefinition().name == query;
        if (path_match || name_match)
            matches.push_back(candidate);
    }
    return matches;
}

struct PortRow {
    std::string name;
    std::string direction;
    std::string domain;
};

/// Renders one port's trace outcome as a single cell.
///
/// Clock names come first when there are any; the parenthesised notes describe
/// why a trace produced nothing, so an empty result is never silently blank.
std::string render_domain(const clock_domain::PortDomain& domain) {
    if (domain.direction == "iface")
        return "(interface port)";

    std::string text;
    for (const auto& clock : domain.clocks) {
        if (!text.empty())
            text += ", ";
        text += clock;
    }

    auto note = [&text](std::string_view value) {
        if (!text.empty())
            text += " ";
        text += value;
    };

    // "(edge source)" rather than "(clock)": with no clock-vs-reset heuristic an
    // async reset drives an event control too, so calling `rst_ni` a clock would
    // overclaim.  A reset also usually reads as data inside the same flop, which
    // is why this annotates the clock list instead of replacing it.
    if (domain.is_clock)
        note("(edge source)");
    if (domain.reached_latch)
        note("(latch)");
    if (domain.hit_depth_limit)
        note("(depth limit)");
    if (text.empty())
        return domain.reached_nothing ? "(comb feedthrough)" : "(none)";
    return text;
}

std::vector<PortRow> collect_port_rows(const slang::ast::InstanceSymbol& instance) {
    std::vector<PortRow> rows;
    for (const auto& domain : clock_domain::analyze_instance(instance))
        rows.push_back(PortRow{domain.name, domain.direction, render_domain(domain)});
    return rows;
}

void print_table(const std::vector<PortRow>& rows, std::ostream& out) {
    size_t name_width = sizeof("Port") - 1;
    size_t dir_width = sizeof("Dir") - 1;
    size_t domain_width = sizeof("Clock Domain") - 1;
    for (const auto& row : rows) {
        name_width = std::max(name_width, row.name.size());
        dir_width = std::max(dir_width, row.direction.size());
        domain_width = std::max(domain_width, row.domain.size());
    }

    auto pad = [&out](const std::string& text, size_t width) {
        out << text << std::string(width - text.size(), ' ');
    };
    auto rule = [&out](size_t width) { out << std::string(width + 2, '-'); };

    out << "| ";
    pad("Port", name_width);
    out << " | ";
    pad("Dir", dir_width);
    out << " | ";
    pad("Clock Domain", domain_width);
    out << " |\n|";
    rule(name_width);
    out << "|";
    rule(dir_width);
    out << "|";
    rule(domain_width);
    out << "|\n";

    for (const auto& row : rows) {
        out << "| ";
        pad(row.name, name_width);
        out << " | ";
        pad(row.direction, dir_width);
        out << " | ";
        pad(row.domain, domain_width);
        out << " |\n";
    }
}

struct ElaborationErrors {
    size_t count = 0;
    std::vector<std::string> sample;
};

/// Collects elaboration errors.  Unlike the LSP server's semantic diagnostics
/// path this tool does not run in LintMode, so an incomplete filelist shows up
/// as real errors rather than being papered over -- and those errors matter
/// even when the requested instance is found, because slang marks the affected
/// expressions bad and the trace then walks straight past them.
ElaborationErrors collect_elaboration_errors(BuiltCompilation& built) {
    constexpr size_t kMaxSampled = 20;
    ElaborationErrors errors;
    slang::DiagnosticEngine engine(*built.source_manager);
    for (const auto& diagnostic : built.compilation->getAllDiagnostics()) {
        if (engine.getSeverity(diagnostic.code, diagnostic.location) !=
            slang::DiagnosticSeverity::Error)
            continue;
        ++errors.count;
        if (errors.sample.size() < kMaxSampled)
            errors.sample.push_back(engine.formatMessage(diagnostic));
    }
    return errors;
}

void print_errors(const ElaborationErrors& errors) {
    for (const auto& message : errors.sample)
        std::cerr << "error: " << message << "\n";
    if (errors.count > errors.sample.size())
        std::cerr << "... and " << (errors.count - errors.sample.size())
                  << " more elaboration errors\n";
}

} // namespace

int main(int argc, char* argv[]) {
    std::string filelist_arg;
    std::string instance_arg;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-f" || arg == "--filelist") {
            if (i + 1 >= argc) {
                print_usage();
                return 1;
            }
            filelist_arg = argv[++i];
        } else if (arg == "-h" || arg == "--help") {
            print_usage();
            return 0;
        } else if (arg == "--version") {
            std::cout << "lazyverilog-clk " << LAZYVERILOG_VERSION << "\n";
            return 0;
        } else if (!arg.empty() && arg[0] == '-') {
            std::cerr << "Unknown option: " << arg << "\n";
            print_usage();
            return 1;
        } else {
            instance_arg = arg;
        }
    }

    if (instance_arg.empty()) {
        print_usage();
        return 1;
    }

    CliProject project = resolve_cli_project(std::filesystem::current_path(), filelist_arg);

    Analyzer analyzer;
    index_cli_project(analyzer, project);
    analyzer.wait_for_background_index_idle();

    // Full elaboration, deliberately without CompilationFlags::LintMode: this
    // tool needs slang to pick real top-level modules so that hierarchical
    // instance paths exist at all.  LintMode elaborates every module as its own
    // top and would flatten the hierarchy away.
    slang::ast::CompilationOptions compilation_options;
    auto built = build_compilation(analyzer.compilation_snapshot(), compilation_options,
                                   project.config.compilation.log_timing);

    InstanceCollector collector;
    built.compilation->getRoot().visit(collector);

    const auto errors = collect_elaboration_errors(built);

    auto matches = match_instances(collector.instances, instance_arg);
    if (matches.empty()) {
        std::cerr << "No instance matching '" << instance_arg << "'\n";

        if (collector.instances.empty()) {
            std::cerr << "\nThe design elaborated no instances at all.\n";
        } else {
            constexpr size_t kMaxListed = 20;
            std::cerr << "\nKnown instances:\n";
            for (size_t i = 0; i < collector.instances.size() && i < kMaxListed; ++i)
                std::cerr << "  " << collector.instances[i].path << "\n";
            if (collector.instances.size() > kMaxListed)
                std::cerr << "  ... and " << (collector.instances.size() - kMaxListed) << " more\n";
        }

        if (errors.count > 0) {
            print_errors(errors);
            std::cerr << "\nThe design did not elaborate cleanly (" << errors.count
                      << " errors); the instance may be missing as a result.\n";
        }
        return 1;
    }

    if (matches.size() > 1) {
        std::cerr << "'" << instance_arg << "' is ambiguous; it matches " << matches.size()
                  << " instances:\n";
        for (const auto& match : matches)
            std::cerr << "  " << match.path << "\n";
        std::cerr << "\nRe-run with a full hierarchical path.\n";
        return 1;
    }

    // Warn even though the instance was found.  slang marks expressions that
    // failed to elaborate as bad and the trace walks past them, so a design
    // with errors can print a table that quietly under-reports domains.
    if (errors.count > 0) {
        std::cerr << "warning: the design elaborated with " << errors.count
                  << " errors; clock domains may be incomplete.\n"
                  << "         Run `lazyverilog-lint -f <filelist>` for the details.\n";
    }

    const auto& target = matches.front();
    std::cout << target.path << " (" << target.symbol->getDefinition().name << ")\n";
    print_table(collect_port_rows(*target.symbol), std::cout);
    return 0;
}
