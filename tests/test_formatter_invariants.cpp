// Formatter invariants and option contracts, checked over a corpus.
//
// The example tests in test_formatter.cpp pin outputs one case at a time.
// These check properties every output must have, for every corpus file and
// for many option combinations, so a corner case shows up without anyone
// having written that case down first:
//   - idempotency: formatting the output again changes nothing
//   - alignment: every token AlignPass gave a column renders at that column
//   - tightness: an aligned column is no wider than its widest member needs
//   - line length: a call left on one line past the limit could not break
//   - option contracts: an option changes only the output aspects it owns
//     (docs/dev/formatter-contracts.md)
#include <catch2/catch_test_macros.hpp>

#include "config.hpp"
#include "features/formatter.hpp"
#include "features/formatter_lexer.hpp"
#include "features/formatter_renderer.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

fs::path find_repo_dir(const fs::path& relative) {
    auto dir = fs::current_path();
    while (true) {
        if (fs::exists(dir / relative))
            return dir / relative;
        if (!dir.has_parent_path() || dir == dir.parent_path())
            break;
        dir = dir.parent_path();
    }
    return relative;
}

struct CorpusFile {
    std::string name;
    std::string text;
};

const std::vector<CorpusFile>& corpus() {
    static const std::vector<CorpusFile> files = [] {
        std::vector<CorpusFile> out;
        for (const char* dir : {"tests/fixtures/formatter_corpus", "tests/rtl/hpc60",
                                "tests/rtl/lazyverilog_formatter_corpus/rtl"}) {
            const fs::path root = find_repo_dir(dir);
            if (!fs::is_directory(root))
                continue;
            std::vector<fs::path> paths;
            for (const auto& e : fs::directory_iterator(root))
                if (e.is_regular_file() && (e.path().extension() == ".sv" || e.path().extension() == ".svh"))
                    paths.push_back(e.path());
            std::sort(paths.begin(), paths.end());
            for (const auto& p : paths) {
                std::ifstream in(p, std::ios::binary);
                out.push_back({p.filename().string(),
                               std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>())});
            }
        }
        return out;
    }();
    return files;
}

// ---------------------------------------------------------------------------
// Option table.  One row per option: the values it can take and the output
// aspects it is allowed to change.  The random configs and the contract test
// both draw from it, so a new option added here is covered by both.

enum Aspect : unsigned {
    Breaks = 1, // which tokens share a line
    Blank = 2,  // blank lines between lines
    Indent = 4, // leading whitespace of a line
    Space = 8,  // whitespace between tokens on a line
};

struct Variant {
    std::string label;
    std::function<void(FormatOptions&)> apply;
};

struct Knob {
    std::string name;
    unsigned aspects;
    std::vector<Variant> variants;
};

#define LV_BOOL(field, aspects)                                                                  \
    Knob{#field, aspects,                                                                         \
         {{"true", [](FormatOptions& o) { o.field = true; }},                                    \
          {"false", [](FormatOptions& o) { o.field = false; }}}}
#define LV_VALUES(field, aspects, ...)                                                           \
    [] {                                                                                          \
        Knob k{#field, aspects, {}};                                                              \
        for (auto v : {__VA_ARGS__}) {                                                            \
            std::ostringstream label;                                                             \
            label << v;                                                                           \
            k.variants.push_back({label.str(), [v](FormatOptions& o) { o.field = v; }});          \
        }                                                                                         \
        return k;                                                                                 \
    }()

const std::vector<Knob>& knobs() {
    static const std::vector<Knob> table = {
        // Indent width also moves where lines end, so it may move breaks.
        LV_VALUES(indent_size, Breaks | Indent | Space, 2, 3, 4),
        LV_VALUES(blank_lines_between_items, Blank, 0, 1, 2),
        LV_VALUES(default_indent_level_inside_outmost_block, Breaks | Indent | Space, 0, 1, 2),
        LV_BOOL(tab_align, Space | Indent),

        LV_BOOL(statement.align, Space | Indent),
        LV_BOOL(statement.align_adaptive, Space | Indent),
        LV_VALUES(statement.lhs_min_width, Space | Indent, 1, 8),
        LV_BOOL(statement.begin_newline, Breaks | Blank | Indent | Space),
        LV_BOOL(statement.wrap_end_else_clauses, Breaks | Blank | Indent | Space),

        LV_BOOL(port_declaration.align, Space | Indent),
        LV_BOOL(port_declaration.align_adaptive, Space | Indent),
        LV_VALUES(port_declaration.section1_min_width, Space | Indent, 0, 12),
        LV_VALUES(port_declaration.section2_min_width, Space | Indent, 0, 12),
        LV_VALUES(port_declaration.section3_min_width, Space | Indent, 0, 12),
        LV_VALUES(port_declaration.section4_min_width, Space | Indent, 0, 12),
        LV_VALUES(port_declaration.section5_min_width, Space | Indent, 0, 12),

        LV_BOOL(var_declaration.align, Space | Indent),
        LV_BOOL(var_declaration.align_adaptive, Space | Indent),
        LV_VALUES(var_declaration.section1_min_width, Space | Indent, 0, 10),
        LV_VALUES(var_declaration.section2_min_width, Space | Indent, 0, 30),
        LV_VALUES(var_declaration.section3_min_width, Space | Indent, 0, 30),
        LV_VALUES(var_declaration.section4_min_width, Space | Indent, 0, 10),

        LV_BOOL(instance.align, Space | Indent),
        LV_BOOL(instance.align_adaptive, Space | Indent),
        LV_VALUES(instance.port_indent_level, Indent | Space, 1, 2),
        LV_VALUES(instance.instance_port_name_width, Space | Indent, 1, 16),
        LV_VALUES(instance.instance_port_between_paren_width, Space | Indent, 0, 16),

        LV_VALUES(function_call.break_policy, Breaks | Indent | Space, "auto", "always", "never"),
        LV_VALUES(function_call.line_length, Breaks | Indent | Space, 40, 100),
        LV_VALUES(function_call.arg_count, Breaks | Indent | Space, -1, 3),
        LV_VALUES(function_call.layout, Breaks | Indent | Space, "block", "hanging"),
        // Spacing a call widens it, which may tip it past its line length.
        LV_BOOL(function_call.space_before_paren, Breaks | Indent | Space),
        LV_BOOL(function_call.space_inside_paren, Breaks | Indent | Space),

        LV_VALUES(function_declaration.layout, Breaks | Indent | Space, "block", "hanging"),
        LV_VALUES(function_declaration.line_length, Breaks | Indent | Space, 40, 100),
        LV_BOOL(function_declaration.space_before_paren, Breaks | Indent | Space),

        LV_BOOL(module.non_ansi_port_per_line_enabled, Breaks | Indent | Space),
        LV_VALUES(module.non_ansi_port_per_line, Breaks | Indent | Space, 1, 3),
        LV_BOOL(module.non_ansi_port_max_line_length_enabled, Breaks | Indent | Space),
        LV_VALUES(module.non_ansi_port_max_line_length, Breaks | Indent | Space, 40, 80),
        LV_VALUES(module.parameter_layout, Breaks | Indent | Space, "block", "hanging"),

        LV_BOOL(enum_declaration.align, Space | Indent),
        LV_BOOL(enum_declaration.align_adaptive, Space | Indent),
        LV_VALUES(enum_declaration.enum_name_min_width, Space | Indent, 1, 12),
        LV_VALUES(enum_declaration.enum_value_min_width, Space | Indent, 0, 8),

        LV_BOOL(modport.align, Space | Indent),
        LV_BOOL(modport.align_adaptive, Space | Indent),
        LV_VALUES(modport.direction_min_width, Space | Indent, 1, 8),
        LV_VALUES(modport.signal_min_width, Space | Indent, 0, 12),

        LV_BOOL(spacing.control_keyword_space, Space | Indent),
        LV_BOOL(spacing.space_inside_parens, Space | Indent),
        LV_BOOL(spacing.space_inside_dimension_brackets, Space | Indent),
        LV_BOOL(spacing.space_inside_event_control_parens, Space | Indent),
        LV_VALUES(spacing.binary_operator_spacing, Space | Indent, "both", "none", "before", "after"),
        LV_VALUES(spacing.dimension_binary_operator_spacing, Space | Indent, "both", "none", "before", "after"),
        LV_VALUES(spacing.semicolon_spacing, Space | Indent, "both", "none", "before", "after"),
        LV_VALUES(spacing.range_colon_spacing, Space | Indent, "both", "none", "before", "after"),
        LV_VALUES(spacing.indexed_part_select_spacing, Space | Indent, "both", "none", "before", "after"),
        LV_VALUES(spacing.procedural_event_control_at_spacing, Space | Indent, "both", "none", "before", "after"),
        LV_VALUES(spacing.assignment_operator_spacing, Space | Indent, "both", "none", "before", "after"),
    };
    return table;
}

#undef LV_BOOL
#undef LV_VALUES

struct RunConfig {
    FormatOptions opts;
    std::string label;
};

RunConfig random_config(unsigned seed) {
    std::mt19937 rng(seed);
    RunConfig c{FormatOptions{}, "random_config(" + std::to_string(seed) + ")"};
    for (const Knob& k : knobs()) {
        if (std::uniform_int_distribution<int>(0, 2)(rng) != 0)
            continue;
        const Variant& v = k.variants[std::uniform_int_distribution<size_t>(0, k.variants.size() - 1)(rng)];
        v.apply(c.opts);

    }
    return c;
}

// Default, every aligner on, and seeded random mixes.
std::vector<RunConfig> configs(unsigned random_count) {
    std::vector<RunConfig> out{{FormatOptions{}, "default"}};
    RunConfig aligned{FormatOptions{}, "all-align"};
    aligned.opts.statement.align = aligned.opts.port_declaration.align = aligned.opts.var_declaration.align =
        aligned.opts.instance.align = aligned.opts.enum_declaration.align = aligned.opts.modport.align = true;
    out.push_back(aligned);
    for (unsigned s = 1; s <= random_count; ++s)
        out.push_back(random_config(s));
    return out;
}

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> lines;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(pos, nl - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
        pos = nl + 1;
    }
    return lines;
}

// Collects violations instead of failing at the first, so one run reports
// every broken file and config at once.
struct Report {
    std::vector<std::string> items;
    std::string text() const {
        std::string out;
        for (const auto& s : items) out += s + "\n";
        return out;
    }
    void add(const std::string& what, const CorpusFile& f, const RunConfig& c, int line, const std::string& detail) {
        if (items.size() < 40)
            items.push_back(what + " " + f.name + ":" + std::to_string(line + 1) + " [" + c.label + "]\n    " + detail);
        else if (items.size() == 40)
            items.push_back("...");
    }
};

struct Rendered {
    svfmt::TokenStream tokens;
    svfmt::RenderTrace trace;
    std::string text;
    std::vector<std::string> lines;
};

Rendered render(const std::string& source, const FormatOptions& opts) {
    Rendered r;
    r.tokens = svfmt::TokenCollector(source, opts).collect();
    svfmt::run_format_passes(r.tokens, opts);
    r.text = svfmt::render_tokens(r.tokens, &r.trace);
    r.lines = split_lines(r.text);
    return r;
}

bool at_line_start(const Rendered& r, size_t i) {
    const int line = r.trace.line[i], col = r.trace.column[i];
    if (line < 0 || static_cast<size_t>(line) >= r.lines.size())
        return true;
    const std::string& s = r.lines[line];
    for (int k = 0; k < col && k < static_cast<int>(s.size()); ++k)
        if (s[k] != ' ' && s[k] != '\t')
            return false;
    return true;
}

// Blanks rendered right before token `i` on its line.
int gap_before(const Rendered& r, size_t i) {
    const std::string& s = r.lines[r.trace.line[i]];
    int k = r.trace.column[i];
    while (k > 0 && k <= static_cast<int>(s.size()) && s[k - 1] == ' ') --k;
    return r.trace.column[i] - k;
}

bool visible(const Rendered& r, size_t i) {
    return r.trace.line[i] >= 0 && !svfmt::is_passthrough(r.tokens[i]);
}

// AlignPass promised `target_column`; the renderer only pads up to it, so a
// token past its column means its group was measured too narrow.
void check_alignment(const Rendered& r, const CorpusFile& f, const RunConfig& c, Report& rep) {
    for (size_t i = 0; i < r.tokens.size(); ++i) {
        const auto& a = r.tokens[i].mutable_.align;
        if (!a.enabled || a.target_column < 0 || !visible(r, i) || at_line_start(r, i))
            continue;
        if (r.trace.column[i] != a.target_column)
            rep.add("align-miss", f, c, r.trace.line[i],
                    "`" + r.tokens[i].lex.text + "` at column " + std::to_string(r.trace.column[i]) +
                        ", target " + std::to_string(a.target_column) + ": " + r.lines[r.trace.line[i]]);
    }
}

// With no minimum widths, an aligned column on consecutive lines must touch
// at least one of its members: padding every member is wasted width.
void check_tightness(const Rendered& r, const CorpusFile& f, const RunConfig& c, Report& rep) {
    // target column -> (last line seen, run start line, smallest slack in run)
    struct Run { int last, start, slack; };
    std::map<int, Run> runs;
    auto close = [&](int target, const Run& run) {
        if (run.slack > 0 && run.last > run.start)
            rep.add("align-loose", f, c, run.start,
                    "column " + std::to_string(target) + " padded by " + std::to_string(run.slack) +
                        " on every line " + std::to_string(run.start + 1) + "-" + std::to_string(run.last + 1));
    };
    // A line holding only a comment does not end a run: `// .rst(rst),`
    // between two connections leaves them one column.
    std::vector<bool> code_line(r.lines.size(), false);
    for (size_t i = 0; i < r.tokens.size(); ++i)
        if (r.trace.line[i] >= 0 && svfmt::is_code_token(r.tokens[i]))
            code_line[r.trace.line[i]] = true;
    auto adjacent = [&](int last, int line) {
        for (int k = last + 1; k < line; ++k)
            if (code_line[k]) return false;
        return true;
    };
    for (size_t i = 0; i < r.tokens.size(); ++i) {
        const auto& a = r.tokens[i].mutable_.align;
        if (!a.enabled || a.target_column < 0 || !visible(r, i) || at_line_start(r, i))
            continue;
        const auto d = svfmt::compose(r.tokens[i]);
        // An aligned column keeps at least one blank from what precedes it
        // (`.d (`, `clk ;`), whatever the token's own spacing asks for.
        const int slack = gap_before(r, i) - std::max(d.spaces_before, 1);
        const int line = r.trace.line[i];
        auto it = runs.find(a.target_column);
        if (it != runs.end() && adjacent(it->second.last, line)) {
            it->second.last = line;
            it->second.slack = std::min(it->second.slack, slack);
        } else {
            if (it != runs.end()) close(it->first, it->second);
            runs[a.target_column] = {line, line, slack};
        }
    }
    for (const auto& [target, run] : runs) close(target, run);
}

// break_policy "auto": a call still on one line past the limit is a miss
// when breaking it would have put its arguments inside the limit.  WrapPass
// runs before AlignPass, so alignment padding on the line is not counted.
void check_call_length(const Rendered& r, const CorpusFile& f, const RunConfig& c, Report& rep) {
    const auto& fc = c.opts.function_call;
    if (fc.break_policy != "auto")
        return;
    // A call inside another call's arguments or a bracket is an operand and
    // is never broken on its own.
    std::vector<bool> nested(r.tokens.size(), false);
    std::vector<size_t> lists;
    for (size_t i = 0; i < r.tokens.size(); ++i) {
        const svfmt::Tok& t = r.tokens[i];
        if (!svfmt::is_code_token(t))
            continue;
        if (t.lex.kind == svfmt::TK::OpenParenthesis) {
            nested[i] = !lists.empty() || t.immutable.syntax.bracket_depth > 0;
            if (t.immutable.topology.starts_argument_list || t.immutable.topology.starts_parameter_list)
                lists.push_back(i);
        } else if (t.lex.kind == svfmt::TK::CloseParenthesis && !lists.empty() &&
                   lists.back() == t.immutable.syntax.matching_token) {
            lists.pop_back();
        }
    }
    for (size_t open = 0; open < r.tokens.size(); ++open) {
        const svfmt::Tok& t = r.tokens[open];
        if (!t.immutable.topology.starts_argument_list || !visible(r, open))
            continue;
        const size_t close = t.immutable.syntax.matching_token;
        if (close == svfmt::npos || close >= r.tokens.size() || !visible(r, close) ||
            r.trace.line[close] != r.trace.line[open])
            continue;
        // shortcut: WrapPass lays these out by their own rules (a gate's
        // terminals stay put, a declaration's formals follow
        // [format.function_declaration]); mirrored here, not shared.
        if (svfmt::is_gate_terminal_open(r.tokens, open) || nested[open])
            continue;
        bool is_decl = false;
        for (size_t k = open; k-- > 0;) {
            const auto kind = r.tokens[k].lex.kind;
            if (kind == svfmt::TK::Semicolon) break;
            if (kind == svfmt::TK::FunctionKeyword || kind == svfmt::TK::TaskKeyword) { is_decl = true; break; }
        }
        if (is_decl)
            continue;
        int padding = 0;
        for (size_t k = open; k-- > 0 && r.trace.line[k] == r.trace.line[open];)
            if (r.tokens[k].mutable_.align.enabled && visible(r, k) && !at_line_start(r, k))
                padding += std::max(0, gap_before(r, k) - svfmt::compose(r.tokens[k]).spaces_before);
        const int end = r.trace.column[close] + 1 - padding;
        if (end > fc.line_length && r.trace.column[open] + 1 - padding < fc.line_length)
            rep.add("call-too-long", f, c, r.trace.line[open],
                    "call ends at column " + std::to_string(end) + " > " + std::to_string(fc.line_length) + ": " +
                        r.lines[r.trace.line[open]]);
    }
}

} // namespace

TEST_CASE("formatter invariants hold over the corpus", "[formatter][invariants]") {
    REQUIRE(corpus().size() >= 18);
    Report rep;
    const std::vector<RunConfig> all = configs(24);
    for (const RunConfig& c : all) {
        for (const CorpusFile& f : corpus()) {
            // A big generated file is slow and repeats itself: default only.
            if (f.text.size() >= 64 * 1024 && &c != &all.front())
                continue;
            std::string once;
            try {
                once = format_source(f.text, c.opts);
            } catch (const std::exception& e) {
                rep.add("throws", f, c, 0, e.what());
                continue;
            }
            const std::string twice = format_source(once, c.opts);
            if (twice != once) {
                const auto a = split_lines(once), b = split_lines(twice);
                size_t k = 0;
                while (k < a.size() && k < b.size() && a[k] == b[k]) ++k;
                rep.add("not-idempotent", f, c, static_cast<int>(k),
                        (k < a.size() ? a[k] : "<eof>") + "\n    became: " + (k < b.size() ? b[k] : "<eof>"));
            }
            const Rendered r = render(f.text, c.opts);
            check_alignment(r, f, c, rep);
            check_call_length(r, f, c, rep);
        }
    }
    INFO(rep.text());
    CHECK(rep.items.empty());
}

TEST_CASE("aligned columns are no wider than needed", "[formatter][invariants]") {
    // Every minimum width off, so only the members set a column's width.
    RunConfig tight{FormatOptions{}, "tight"};
    auto& o = tight.opts;
    o.statement.align = o.port_declaration.align = o.var_declaration.align = o.instance.align =
        o.enum_declaration.align = o.modport.align = true;
    o.statement.align_adaptive = o.port_declaration.align_adaptive = o.var_declaration.align_adaptive =
        o.instance.align_adaptive = o.enum_declaration.align_adaptive = o.modport.align_adaptive = false;
    o.statement.lhs_min_width = 0;
    o.port_declaration.section1_min_width = o.port_declaration.section2_min_width =
        o.port_declaration.section3_min_width = o.port_declaration.section4_min_width =
            o.port_declaration.section5_min_width = 0;
    o.var_declaration.section1_min_width = o.var_declaration.section2_min_width =
        o.var_declaration.section3_min_width = o.var_declaration.section4_min_width = 0;
    o.instance.instance_port_name_width = o.instance.instance_port_between_paren_width = 0;
    o.enum_declaration.enum_name_min_width = o.enum_declaration.enum_value_min_width = 0;
    o.modport.direction_min_width = o.modport.signal_min_width = 0;

    Report rep;
    for (const CorpusFile& f : corpus())
        check_tightness(render(f.text, o), f, tight, rep);
    INFO(rep.text());
    CHECK(rep.items.empty());
}

namespace {

// The aspects of an output that differ from a reference output.  Lines are
// matched by their text with all whitespace removed; once those differ the
// breaks changed and nothing finer can be compared.
unsigned changed_aspects(const std::string& a, const std::string& b, int& where) {
    struct Shape {
        std::vector<std::string> text;
        std::vector<int> blank, indent;
        std::vector<std::string> body;
    };
    auto shape = [](const std::string& s) {
        Shape out;
        int blanks = 0;
        for (const std::string& line : split_lines(s)) {
            size_t lead = line.find_first_not_of(" \t");
            if (lead == std::string::npos) { ++blanks; continue; }
            std::string squeezed;
            for (char ch : line) if (ch != ' ' && ch != '\t') squeezed += ch;
            out.text.push_back(squeezed);
            out.blank.push_back(blanks);
            out.indent.push_back(static_cast<int>(lead));
            out.body.push_back(line.substr(lead));
            blanks = 0;
        }
        return out;
    };
    const Shape x = shape(a), y = shape(b);
    if (x.text != y.text) {
        size_t k = 0;
        while (k < x.text.size() && k < y.text.size() && x.text[k] == y.text[k]) ++k;
        where = static_cast<int>(k);
        return Breaks;
    }
    unsigned out = 0;
    where = -1;
    for (size_t k = 0; k < x.text.size(); ++k) {
        unsigned here = (x.blank[k] != y.blank[k] ? Blank : 0) | (x.indent[k] != y.indent[k] ? Indent : 0) |
                        (x.body[k] != y.body[k] ? Space : 0);
        if (here & ~out && where < 0) where = static_cast<int>(k);
        out |= here;
    }
    return out;
}

std::string aspect_names(unsigned a) {
    std::string s;
    for (auto [bit, name] : {std::pair{Breaks, "breaks"}, {Blank, "blank"}, {Indent, "indent"}, {Space, "space"}})
        if (a & bit) s += s.empty() ? name : std::string(",") + name;
    return s;
}

} // namespace

TEST_CASE("every option changes only the aspects it owns", "[formatter][contracts]") {
    // docs/dev/formatter-contracts.md lists the same table in prose.
    Report rep;
    // The small style files only: every option times every file is the
    // slowest test here, and the generated blocks add no new shapes.
    std::vector<CorpusFile> files;
    for (const CorpusFile& f : corpus())
        if (f.name.rfind("hpc_", 0) != 0 && f.text.size() < 64 * 1024) files.push_back(f);
    for (const RunConfig& base : configs(6)) {
        std::vector<std::string> reference;
        for (const CorpusFile& f : files) reference.push_back(format_source(f.text, base.opts));
        for (const Knob& k : knobs()) {
            for (const Variant& v : k.variants) {
                FormatOptions opts = base.opts;
                v.apply(opts);
                for (size_t n = 0; n < files.size(); ++n) {
                    int where = -1;
                    const unsigned changed = changed_aspects(reference[n], format_source(files[n].text, opts), where);
                    if (changed & ~k.aspects)
                        rep.add("contract", files[n], base, where,
                                k.name + "=" + v.label + " changed " + aspect_names(changed & ~k.aspects) +
                                    " (owns " + aspect_names(k.aspects) + ")");
                }
            }
        }
    }
    INFO(rep.text());
    CHECK(rep.items.empty());
}

