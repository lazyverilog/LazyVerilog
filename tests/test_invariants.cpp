// Properties every feature must hold on any real design, checked over a corpus
// rather than on hand-written fixtures.  Default corpus: tests/rtl/hpc60;
// override with LAZYVERILOG_INVARIANT_CORPUS.  Known violations live in
// tests/invariants_exclusions.txt as `<invariant> <path relative to corpus>  # reason`.
#include "analyzer.hpp"
#include "features/folding_range.hpp"
#include "features/formatter.hpp"
#include "features/rename.hpp"
#include "string_utils.hpp"
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <tuple>

namespace fs = std::filesystem;

namespace {

std::string read_text(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

bool is_source(const fs::path& path) {
    const auto ext = path.extension();
    return ext == ".sv" || ext == ".svh" || ext == ".v";
}

std::vector<fs::path> sources_in(const fs::path& dir) {
    std::vector<fs::path> out;
    for (const auto& entry : fs::recursive_directory_iterator(dir))
        if (entry.is_regular_file() && is_source(entry.path()))
            out.push_back(entry.path());
    std::sort(out.begin(), out.end());
    return out;
}

// Each immediate subdirectory is one project, unless the corpus holds sources itself.
std::vector<fs::path> projects_in(const fs::path& corpus) {
    for (const auto& entry : fs::directory_iterator(corpus))
        if (entry.is_regular_file() && is_source(entry.path()))
            return {corpus};
    std::vector<fs::path> out;
    for (const auto& entry : fs::directory_iterator(corpus))
        if (entry.is_directory())
            out.push_back(entry.path());
    std::sort(out.begin(), out.end());
    return out;
}

std::set<std::pair<std::string, std::string>> load_exclusions(const fs::path& file) {
    std::set<std::pair<std::string, std::string>> out;
    std::ifstream in(file);
    std::string line;
    while (std::getline(in, line)) {
        line = line.substr(0, line.find('#'));
        std::istringstream words(line);
        std::string invariant, path;
        if (words >> invariant >> path)
            out.emplace(invariant, path);
    }
    return out;
}

bool is_word(char c) {
    return std::isalnum((unsigned char)c) || c == '_' || c == '$';
}

// Word starts where identifier_at() names something, at most 8, spread evenly.
// Every request walks the file's tree (~8 ms here), so word starts are thinned
// to 24 before asking and the whole run stays near a minute on hpc60.
std::vector<std::pair<int, int>> identifier_positions(const Analyzer& analyzer,
                                                      const std::string& uri,
                                                      const std::string& text) {
    std::vector<std::pair<int, int>> starts;
    int line = 0, col = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if ((std::isalpha((unsigned char)c) || c == '_') && (i == 0 || !is_word(text[i - 1])))
            starts.emplace_back(line, col);
        if (c == '\n') {
            ++line;
            col = 0;
        } else {
            ++col;
        }
    }
    std::vector<std::pair<int, int>> named;
    for (size_t i = 0, k = std::max<size_t>(1, (starts.size() + 23) / 24); i < starts.size(); i += k)
        if (analyzer.identifier_at(uri, starts[i].first, starts[i].second))
            named.push_back(starts[i]);
    std::vector<std::pair<int, int>> out;
    for (size_t i = 0, k = std::max<size_t>(1, (named.size() + 7) / 8); i < named.size(); i += k)
        out.push_back(named[i]);
    return out;
}

using Span = std::tuple<std::string, int, int, int, int>;

std::string show(const std::set<Span>& spans) {
    std::string out;
    for (const auto& [uri, l, c, el, ec] : spans)
        out += uri + ":" + std::to_string(l) + ":" + std::to_string(c) + "-" +
               std::to_string(el) + ":" + std::to_string(ec) + "\n";
    return out;
}

std::string show(const std::optional<Location>& loc) {
    if (!loc)
        return "none";
    return show(std::set<Span>{{loc->uri, loc->line, loc->col, loc->end_line, loc->end_col}});
}

bool same_place(const std::optional<Location>& a, const std::optional<Location>& b) {
    if (!a || !b)
        return a.has_value() == b.has_value();
    return std::tie(a->uri, a->line, a->col) == std::tie(b->uri, b->line, b->col);
}

} // namespace

TEST_CASE("invariants: format, folding, rename/references, definition over a corpus",
          "[invariants]") {
    const char* env = std::getenv("LAZYVERILOG_INVARIANT_CORPUS");
    const fs::path corpus = env && *env ? fs::path(env)
                                        : fs::path(LAZYVERILOG_SOURCE_DIR) / "tests/rtl/hpc60";
    REQUIRE(fs::is_directory(corpus));
    const auto excluded =
        load_exclusions(fs::path(LAZYVERILOG_SOURCE_DIR) / "tests/invariants_exclusions.txt");

    for (const auto& project : projects_in(corpus)) {
        const auto files = sources_in(project);
        std::vector<std::string> paths;
        for (const auto& f : files)
            paths.push_back(f.string());

        INFO("project " << project.string());
        REQUIRE_FALSE(files.empty());
        // What actually got checked: each check below is conditional, so without
        // these a regression to "names nothing" would pass by checking nothing.
        int positions = 0, renames_with_edits = 0, backlinks = 0;

        Analyzer analyzer;
        analyzer.set_extra_files(paths);
        analyzer.wait_for_background_index_idle();
        // One buffer open at a time, as in an editor: the rest are answered from
        // their shards, which is the path closed files take for real.
        for (size_t i = 0; i < files.size(); ++i) {
            const std::string uri  = uri_from_path(files[i]);
            const std::string text = read_text(files[i]);
            analyzer.open(uri, text);
            struct Closer {
                Analyzer& a;
                const std::string& u;
                ~Closer() { a.close(u); }
            } closer{analyzer, uri};
            const std::string rel = fs::relative(files[i], corpus).generic_string();
            const auto skip = [&](const char* invariant) {
                return excluded.contains({invariant, rel});
            };

            if (!skip("format")) {
                INFO("invariant format, file " << rel);
                std::string once;
                REQUIRE_NOTHROW(once = format_source(text, FormatOptions{}));
                CHECK(format_source(once, FormatOptions{}) == once);
            }

            if (!skip("folding")) {
                FoldingRangeRequestParams params;
                params.textDocument.uri.raw_uri_ = uri;
                const auto folds = provide_folding_range(analyzer, params);
                const int line_count = (int)std::count(text.begin(), text.end(), '\n') + 1;
                for (size_t a = 0; a < folds.size(); ++a) {
                    const auto& fa = folds[a];
                    INFO("invariant folding, file " << rel << ", fold " << fa.startLine << "-"
                                                    << fa.endLine);
                    CHECK(fa.startLine <= fa.endLine);
                    CHECK(fa.endLine < line_count);
                    for (size_t b = a + 1; b < folds.size(); ++b) {
                        const auto& fb = folds[b];
                        // Sharing one delimiter line counts as disjoint: `) (` in a
                        // parameterized header ends one list and starts the next.
                        const bool disjoint =
                            fa.endLine <= fb.startLine || fb.endLine <= fa.startLine;
                        const bool nested =
                            (fa.startLine <= fb.startLine && fb.endLine <= fa.endLine) ||
                            (fb.startLine <= fa.startLine && fa.endLine <= fb.endLine);
                        if (!disjoint && !nested)
                            FAIL_CHECK("overlaps fold " << fb.startLine << "-" << fb.endLine);
                    }
                }
            }

            if (skip("rename-references") && skip("definition-backlink"))
                continue;
            for (const auto& [line, col] : identifier_positions(analyzer, uri, text)) {
                ++positions;
                const auto refs = analyzer.find_references(uri, line, col, true);
                std::set<Span> ref_spans;
                for (const auto& r : refs)
                    ref_spans.emplace(r.uri, r.line, r.col, r.end_line, r.end_col);

                if (!skip("rename-references")) {
                    const auto name = analyzer.identifier_at(uri, line, col)->name;
                    TextDocumentRename::Params params;
                    params.textDocument.uri.raw_uri_ = uri;
                    params.position = lsPosition(line, col);
                    params.newName = name + "_rn";
                    const auto edit = provide_rename(analyzer, params);
                    std::set<Span> edit_spans;
                    if (edit.changes)
                        for (const auto& [edit_uri, edits] : *edit.changes)
                            for (const auto& e : edits)
                                edit_spans.emplace(edit_uri, e.range.start.line,
                                                   e.range.start.character, e.range.end.line,
                                                   e.range.end.character);
                    INFO("invariant rename-references, file " << rel << ", position " << line
                                                              << ":" << col << " (" << name << ")");
                    INFO("rename edits:\n" << show(edit_spans) << "references:\n"
                                           << show(ref_spans));
                    if (!edit_spans.empty()) {
                        ++renames_with_edits;
                        CHECK(edit_spans == ref_spans);
                        // The edits are built from find_references(), so the check
                        // above holds by construction.  What can fail is the set
                        // seen from another occurrence: renaming from any use must
                        // rewrite the same spans.  Sampled (first, middle, last) in
                        // the open buffer, the only file asked from here.
                        std::vector<Location> local;
                        for (const auto& r : refs)
                            if (r.uri == uri && r.form == RefForm::Plain)
                                local.push_back(r);
                        for (size_t k : {size_t{0}, local.size() / 2, local.size() - 1}) {
                            if (local.empty())
                                break;
                            const auto& r = local[k];
                            std::set<Span> from_there;
                            for (const auto& o : analyzer.find_references(r.uri, r.line, r.col, true))
                                from_there.emplace(o.uri, o.line, o.col, o.end_line, o.end_col);
                            INFO("seen from " << r.line << ":" << r.col << ":\n" << show(from_there));
                            CHECK(from_there == edit_spans);
                        }
                    }
                }

                if (!skip("definition-backlink")) {
                    const auto d = analyzer.definition_of(uri, line, col);
                    if (!d)
                        continue;
                    for (const auto& r : refs) {
                        // definition_of() answers for open buffers only.
                        if (r.uri != uri)
                            continue;
                        // `.p,` means the instance's port *and* this module's p;
                        // its definition is the port, so it cannot lead back here.
                        if (r.form != RefForm::Plain)
                            continue;
                        ++backlinks;
                        const auto back = analyzer.definition_of(r.uri, r.line, r.col);
                        INFO("invariant definition-backlink, file " << rel << ", position "
                                                                    << line << ":" << col);
                        INFO("definition: " << show(d) << "from reference " << r.uri << ":"
                                            << r.line << ":" << r.col << ": " << show(back));
                        CHECK(same_place(back, d));
                    }
                }
            }
        }
        // shortcut: assumes no exclusion covers a whole project, revisit if one must.
        CHECK(positions > 0);
        CHECK(renames_with_edits > 0);
        CHECK(backlinks > 0);
    }
}
