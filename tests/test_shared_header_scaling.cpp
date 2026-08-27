// Guards the cost model of a `include`d header that every file in a project
// shares.
//
// A design where 60 modules each `include` one large header re-parses that
// header 60 times by construction — a header is a textual fragment whose
// expansion depends on the includer, so it cannot be indexed once and reused
// (see PERF.md).  What must *not* happen is per-file *index* work proportional
// to the header's size on top of that: at 60 includers, an O(header) step run
// once per file is a 60x tax on the largest thing in the project.
//
// Two such steps regressed and are pinned here:
//
//   * building the object-like-macro alias table from every `define the tree
//     knows, whether or not any indexed type spells a macro;
//   * keying every module-scoped typedef into package_type_by_scoped_name,
//     which only `pkg::t` / `cls::t` lookups ever read.
//
// The timing check compares two headers that differ only in the *shape* of
// their macro bodies — same file size, same macro count, same declarations —
// so parsing, preprocessing and every other pass cancel out and no absolute
// threshold is needed.  Both halves run back to back on the same machine,
// which is what makes this safe on a shared CI runner.
#include "syntax_index.hpp"
#include "syntax_index_shared.hpp"

#include <catch2/catch_test_macros.hpp>
#include <slang/syntax/SyntaxTree.h>
#include <slang/text/SourceManager.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

// Declarations shared by both halves of the timing comparison: enough entries
// that one SyntaxIndex::build() is comfortably above timer resolution.
constexpr int kTypedefCount = 300;
// Macros the indexed declarations never mention.  Realistic for an RTL
// project's central header, which defines far more than any one module uses.
constexpr int kMacroCount = 30000;
constexpr int kTimedRuns = 11;

std::string typedef_body(int count) {
    std::string text;
    for (int i = 0; i < count; ++i) {
        const auto n = std::to_string(i);
        text += "typedef struct packed {\n    logic [7:0] f_a_" + n + ";\n    logic       f_b_" +
                n + ";\n} rec_" + n + "_t;\n";
    }
    return text;
}

/// @p body_prefix picks whether each macro body is a bare identifier — the one
/// shape the type-alias table can substitute — or a literal it must ignore.
std::string macro_definitions(int count, const std::string& body_prefix) {
    std::string text;
    for (int i = 0; i < count; ++i) {
        const auto n = std::to_string(i);
        text += "`define CFG_NAME_" + n + " " + body_prefix + n + "\n";
    }
    return text;
}

/// One throwaway project directory: a header plus a single module that
/// `include`s it inside its body, which is the shape this test is about.
class SharedHeaderProject {
  public:
    SharedHeaderProject(const std::string& tag, const std::string& header_text) {
        dir_ = std::filesystem::temp_directory_path() / ("lazyverilog-shared-header-" + tag);
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(dir_);

        write(dir_ / "shared.svh", header_text);
        write(dir_ / "blk.sv", "module blk (input logic clk_i);\n"
                               "`include \"shared.svh\"\n"
                               "endmodule\n");
    }

    ~SharedHeaderProject() {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    SharedHeaderProject(const SharedHeaderProject&) = delete;
    SharedHeaderProject& operator=(const SharedHeaderProject&) = delete;

    std::filesystem::path module_path() const { return dir_ / "blk.sv"; }
    const std::filesystem::path& dir() const { return dir_; }

  private:
    static void write(const std::filesystem::path& path, const std::string& text) {
        std::ofstream out(path, std::ios::binary);
        out << text;
    }

    std::filesystem::path dir_;
};

/// Fastest wall time of SyntaxIndex::build() alone, on the shard path background
/// project indexing uses: Declarations depth, restricted to the module's own
/// file.  IndexDepth::Full walks the macro table on purpose — it populates
/// index.macros for the current file — so it would drown out what is measured
/// here.
///
/// Parsing stays outside the timed region: preprocessing N macros legitimately
/// costs O(N), and charging that to the index would hide the per-file cost.
double fastest_build_ms(const SharedHeaderProject& project, int runs, size_t& typedefs_out) {
    auto sm = make_lsp_source_manager();
    (void)sm->addUserDirectories(project.dir().string());

    auto tree_or_error = slang::syntax::SyntaxTree::fromFile(project.module_path().string(), *sm);
    REQUIRE(tree_or_error);
    auto tree = *tree_or_error;
    REQUIRE(tree != nullptr);

    const auto root_buffer = tree->root().sourceRange().start().buffer();
    const auto source = sm->getSourceText(root_buffer);
    // Take the URI from the buffer rather than the path so it is byte-identical
    // to what the resolver derives for the same buffer; a mismatch would silently
    // restrict the build to nothing.
    const auto uri = uri_from_source_buffer(*sm, root_buffer);
    REQUIRE(!uri.empty());

    std::vector<double> samples;
    samples.reserve(static_cast<size_t>(runs));
    for (int i = 0; i < runs; ++i) {
        const auto start = Clock::now();
        auto index = SyntaxIndex::build(*tree, source, IndexDepth::Declarations, uri);
        const auto elapsed = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        typedefs_out = index.typedefs.size();
        samples.push_back(elapsed);
    }
    std::sort(samples.begin(), samples.end());
    // The fastest run, not the median: the cost this test is about is a fixed
    // amount of extra work, so it shows up as a raised floor.  Everything a
    // shared CI runner adds on top is noise that only ever makes a sample
    // slower, and taking the minimum discards all of it.
    return samples.front();
}

} // namespace

TEST_CASE("shared header: a module-scoped typedef adds no scoped-lookup key", "[index][scaling]") {
    // package_type_by_scoped_name answers `owner::name`, and nothing can name a
    // module's typedef that way.  Keying them anyway costs one key per typedef
    // per including file — the whole header duplicated across the project index
    // for entries no lookup can reach.
    const std::string module_text =
        "module blk (input logic clk_i);\n" + typedef_body(8) + "endmodule\n";
    auto module_tree = slang::syntax::SyntaxTree::fromText(module_text);
    REQUIRE(module_tree != nullptr);
    const auto module_index = SyntaxIndex::build(*module_tree, module_text);
    CHECK(module_index.typedefs.size() == 8);
    CHECK(module_index.package_type_by_scoped_name.empty());

    // The two owners that *are* addressable keep their keys: without them every
    // class's `type_id` collapses onto whichever one was indexed first.
    const std::string scoped_text = R"(
package cfg_pkg;
    typedef logic [7:0] byte_t;
endpackage

class my_item;
    typedef my_item type_id;
endclass
)";
    auto scoped_tree = slang::syntax::SyntaxTree::fromText(scoped_text);
    REQUIRE(scoped_tree != nullptr);
    const auto scoped_index = SyntaxIndex::build(*scoped_tree, scoped_text);
    CHECK(scoped_index.package_type_by_scoped_name.contains(
        package_scoped_key("cfg_pkg", "byte_t")));
    CHECK(scoped_index.package_type_by_scoped_name.contains(
        package_scoped_key("my_item", "type_id")));
}

TEST_CASE("shared header: a macro-spelled type still resolves to its alias", "[index][scaling]") {
    // The alias table is now built only when some indexed type spells a macro.
    // This is the case that has to keep finding it.
    const std::string text = R"(
`define ITEM_T my_item

class my_item;
endclass

class holder;
    `ITEM_T item_h;
endclass
)";
    auto tree = slang::syntax::SyntaxTree::fromText(text);
    REQUIRE(tree != nullptr);
    const auto index = SyntaxIndex::build(*tree, text);

    REQUIRE(index.class_by_name.contains("holder"));
    const auto& holder = index.classes[index.class_by_name.at("holder")];
    REQUIRE(holder.fields.size() == 1);
    CHECK(holder.fields[0].type == "my_item");
}

TEST_CASE("shared header: index build cost is flat in unreferenced macro count",
          "[index][scaling]") {
    const std::string declarations = typedef_body(kTypedefCount);
    // Identical in size and macro count; only the body shape differs, so the
    // alias table is the sole pass that can tell the two apart.
    const SharedHeaderProject literal_bodies(
        "literal", macro_definitions(kMacroCount, "32'd") + declarations);
    const SharedHeaderProject identifier_bodies(
        "identifier", macro_definitions(kMacroCount, "cfg_value_") + declarations);

    size_t literal_typedefs = 0;
    size_t identifier_typedefs = 0;

    // Warm the allocator and the instruction cache so the first-measured half is
    // not charged for both.
    (void)fastest_build_ms(literal_bodies, 2, literal_typedefs);

    const double literal_ms = fastest_build_ms(literal_bodies, kTimedRuns, literal_typedefs);
    const double identifier_ms =
        fastest_build_ms(identifier_bodies, kTimedRuns, identifier_typedefs);

    // Same declarations on both sides, or the comparison is not like for like.
    REQUIRE(literal_typedefs == static_cast<size_t>(kTypedefCount));
    REQUIRE(identifier_typedefs == literal_typedefs);

    std::cout << "\n[shared-header scaling] typedefs=" << kTypedefCount
              << " macros=" << kMacroCount << " build_literal_bodies_ms=" << literal_ms
              << " build_identifier_bodies_ms=" << identifier_ms
              << " ratio=" << (identifier_ms / literal_ms) << "\n";

    // A generous factor: the point is to catch a pass that scales with the macro
    // table, not to police a few percent of noise on a shared runner.
    CHECK(identifier_ms < literal_ms * 2.0);
}

// ── The shard a shared header gets ──────────────────────────────────────────
//
// Since shard scoping, an includer's shard carries only its own declarations,
// so the header's own shard is the only place its declarations exist and the
// only thing every other file resolves against.  background_index_loop() built
// that shard from whichever includer happened to claim the header first,
// restricted to the header's URI -- and SourceFileIdResolver::wants_declaration()
// keeps every declaration when no mentions provider is set, so the restriction
// did not actually filter.  The result was a copy of an includer's declarations
// under the header's URI.
#include "analyzer.hpp"
#include "string_utils.hpp"

#include <set>

namespace {

/// N modules that all `include` one header.
class SharedHeaderDesign {
  public:
    SharedHeaderDesign(std::string tag, int modules, const std::string& header_text,
                       const std::string& module_template) {
        dir_ = std::filesystem::temp_directory_path() / ("lazyverilog-design-" + tag);
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(dir_);
        dir_ = std::filesystem::canonical(dir_);

        write(dir_ / "defs.svh", header_text);
        for (int i = 0; i < modules; ++i) {
            const auto name = "blk_" + std::to_string(i);
            std::string text = module_template;
            for (size_t pos = text.find("@NAME@"); pos != std::string::npos;
                 pos = text.find("@NAME@"))
                text.replace(pos, 6, name);
            const auto path = dir_ / (name + ".sv");
            write(path, text);
            module_paths_.push_back(path.string());
        }
    }

    ~SharedHeaderDesign() {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    SharedHeaderDesign(const SharedHeaderDesign&) = delete;
    SharedHeaderDesign& operator=(const SharedHeaderDesign&) = delete;

    std::string header_uri() const { return uri_from_path(dir_ / "defs.svh"); }
    size_t module_count() const { return module_paths_.size(); }
    std::string module_uri(size_t i) const { return uri_from_path(module_paths_[i]); }

    /// An extra header beside defs.svh, for the nested-`include case.
    void write_extra_header(const std::string& name, const std::string& text) {
        write(dir_ / name, text);
    }

    /// Cold index of the whole design, in milliseconds.
    double index_ms(Analyzer& analyzer) const {
        analyzer.set_project_index_publish_debounce_ms(0);
        const auto start = Clock::now();
        analyzer.set_project_config({}, {dir_.string()}, module_paths_);
        analyzer.wait_for_background_index_idle();
        return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    }

  private:
    static void write(const std::filesystem::path& path, const std::string& text) {
        std::ofstream out(path, std::ios::binary);
        out << text;
    }

    std::filesystem::path dir_;
    std::vector<std::string> module_paths_;
};

std::string localparam_body(int count) {
    std::string text;
    for (int i = 0; i < count; ++i)
        text += "localparam int HDR_" + std::to_string(i) + " = " + std::to_string(i) + ";\n";
    return text;
}

/// `include at file scope -- the layout an RTL tree with a central header uses.
const char* kIncludingModule = "`include \"defs.svh\"\n"
                               "module @NAME@ (input logic clk_i, output logic q_o);\n"
                               "    assign q_o = clk_i ^ (HDR_0 != 0);\n"
                               "endmodule\n";

std::set<std::string> project_module_names(const Analyzer& analyzer) {
    std::set<std::string> names;
    for (const auto& [name, ref] : analyzer.project_index_snapshot()->module_by_name)
        names.insert(name);
    return names;
}

std::set<std::string> shard_values(const Analyzer& analyzer, const std::string& uri) {
    std::set<std::string> names;
    for (const auto& entry : *analyzer.extra_index_snapshot_ptr()) {
        if (entry.uri != uri)
            continue;
        for (const auto& value : entry.index_ref().values)
            names.insert(value.name);
    }
    return names;
}

std::set<std::string> shard_modules(const Analyzer& analyzer, const std::string& uri) {
    std::set<std::string> names;
    for (const auto& entry : *analyzer.extra_index_snapshot_ptr()) {
        if (entry.uri != uri)
            continue;
        for (const auto& module : entry.index_ref().modules)
            names.insert(module.name);
    }
    return names;
}

} // namespace

TEST_CASE("header shard: holds the declarations the header itself makes", "[index][scaling]") {
    SharedHeaderDesign design("hdrshard-values", 4, localparam_body(3), kIncludingModule);
    Analyzer analyzer;
    (void)design.index_ms(analyzer);

    const auto values = shard_values(analyzer, design.header_uri());
    CHECK(values.contains("HDR_0"));
    CHECK(values.contains("HDR_1"));
    CHECK(values.contains("HDR_2"));
}

TEST_CASE("header shard: does not hold an includer's module", "[index][scaling]") {
    SharedHeaderDesign design("hdrshard-modules", 4, localparam_body(3), kIncludingModule);
    Analyzer analyzer;
    (void)design.index_ms(analyzer);

    // Every module is indexed, each under its own URI.
    CHECK(project_module_names(analyzer).size() == design.module_count());
    // None of them belongs to the header.  A module appearing here also lets the
    // snapshot's module_by_name resolve that module to the header's shard.
    CHECK(shard_modules(analyzer, design.header_uri()).empty());
}

TEST_CASE("header shard: a header that cannot stand alone still indexes its includers",
          "[index][scaling]") {
    // A port-list fragment is valid only spliced into its includer -- the case
    // PERF.md warns cannot be indexed standalone.  Whatever the header shard is
    // built from must not break these files.
    SharedHeaderDesign design("hdrshard-fragment", 4,
                              "    input logic clk_i,\n"
                              "    output logic q_o\n",
                              "module @NAME@ (\n"
                              "`include \"defs.svh\"\n"
                              ");\n"
                              "    assign q_o = clk_i;\n"
                              "endmodule\n");
    Analyzer analyzer;
    (void)design.index_ms(analyzer);

    CHECK(project_module_names(analyzer).size() == design.module_count());
}

// ── What each includer has to re-read of a shared header ────────────────────
//
// A header is re-parsed once per includer, and that is the cost model this file
// opens with.  It is only irreducible for the *directives* a header carries:
// what a `define expands to depends on where it is used, so the preprocessor has
// to see them again for every file.  Its declarations do not depend on the
// includer at all, and once the header's own shard records them, re-reading them
// N times buys nothing.
//
// The saving is not total, and the test says so.  The projection can only be
// installed once some file has parsed the header and proved it stands alone, and
// every worker running at that moment is already reading it — so the header's
// bulk is read at most once per background worker, a bound that does not grow
// with the project.  On the one-core slice this is aimed at, that is one read.
//
// So the ratio taken here is between two file counts at the *same* header, which
// is exactly the claim: what a further includer costs no longer depends on how
// big the header is.  A big-header-vs-small-header ratio would instead measure
// that fixed remainder, and would move with the runner's core count.
constexpr int kProjectionHeaderDecls = 4000;
constexpr int kProjectionManyModules = 200;
constexpr int kProjectionFewModules = 25;
constexpr int kProjectionRuns = 3;

/// Fastest cold index of @p design over @p runs, each on a fresh Analyzer.
double fastest_index_ms(const SharedHeaderDesign& design, int runs) {
    std::vector<double> samples;
    samples.reserve(static_cast<size_t>(runs));
    for (int i = 0; i < runs; ++i) {
        Analyzer analyzer;
        samples.push_back(design.index_ms(analyzer));
    }
    std::sort(samples.begin(), samples.end());
    // The minimum, for the same reason fastest_build_ms() takes it: what is
    // measured is a raised floor, and everything a loaded runner adds is noise
    // in one direction only.
    return samples.front();
}

TEST_CASE("shared header: indexing cost stops scaling with the includer count",
          "[index][scaling]") {
    const auto header = localparam_body(kProjectionHeaderDecls);
    const SharedHeaderDesign many("projection-many", kProjectionManyModules, header,
                                  kIncludingModule);
    const SharedHeaderDesign few("projection-few", kProjectionFewModules, header,
                                 kIncludingModule);

    // Warm the page cache and the allocator so the first-measured design is not
    // charged for both.
    {
        Analyzer warm;
        (void)few.index_ms(warm);
    }
    const double few_ms = fastest_index_ms(few, kProjectionRuns);
    const double many_ms = fastest_index_ms(many, kProjectionRuns);

    const double file_ratio =
        static_cast<double>(kProjectionManyModules) / kProjectionFewModules;
    std::cout << "\n[header projection] header_decls=" << kProjectionHeaderDecls
              << " many_modules=" << kProjectionManyModules << " few_modules="
              << kProjectionFewModules << " many_ms=" << many_ms << " few_ms=" << few_ms
              << " ratio=" << (many_ms / few_ms) << " file_ratio=" << file_ratio << "\n";

    // Re-reading the header per file puts this at the file ratio itself, 8x.
    // Generous below that: the point is to catch the return of O(files x header)
    // work, not to police noise on a shared runner.
    CHECK(many_ms < few_ms * 3.0);

    // The saving must not come from indexing less: the header's declarations
    // still have to reach the project index, through the header's own shard.
    Analyzer analyzer;
    (void)many.index_ms(analyzer);
    const auto values = shard_values(analyzer, many.header_uri());
    CHECK(values.contains("HDR_0"));
    CHECK(values.contains("HDR_" + std::to_string(kProjectionHeaderDecls - 1)));
}

TEST_CASE("shared header: an includer's shard stops carrying the header's declarations",
          "[index][scaling]") {
    // The contract this buys the speed with.  An includer's shard used to keep
    // every header declaration the file mentions; now the header's own shard is
    // where they live and what every other file resolves against.
    //
    // The bound is "far fewer than all", not "none": the files already parsing
    // when the projection is installed still see the whole header, so up to one
    // file per background worker keeps its mentioned declarations.  That is a
    // duplicate of what the header shard holds, which is the state every
    // includer was in before, so nothing resolves differently either way.
    constexpr int kModules = 60;
    SharedHeaderDesign design("projection-contract", kModules,
                              "localparam int SHARED_USED = 1;\n" + localparam_body(400),
                              "`include \"defs.svh\"\n"
                              "module @NAME@ (input logic clk_i, output logic q_o);\n"
                              "    assign q_o = clk_i ^ (SHARED_USED != 0);\n"
                              "endmodule\n");
    Analyzer analyzer;
    (void)design.index_ms(analyzer);

    CHECK(shard_values(analyzer, design.header_uri()).contains("SHARED_USED"));

    size_t carriers = 0;
    for (size_t i = 0; i < design.module_count(); ++i) {
        if (shard_values(analyzer, design.module_uri(i)).contains("SHARED_USED"))
            ++carriers;
    }
    INFO("includer shards still carrying SHARED_USED: " << carriers << " of " << kModules);
    CHECK(carriers < design.module_count() / 2);
}

TEST_CASE("shared header: a macro the header defines still reaches its includers",
          "[index][scaling]") {
    // Whatever an includer is spared of a header, it cannot be spared its
    // directives: a macro body is only meaningful where it expands, so dropping
    // one silently deletes every declaration it makes.  A nested `include is the
    // same rule one level down -- drop the directive and the inner header's
    // macros go with it.
    SharedHeaderDesign design("projection-macro", 12,
                              "`include \"inner.svh\"\n" + localparam_body(200) +
                                  "`define DECLARE_TAG logic tag_q\n",
                              "`include \"defs.svh\"\n"
                              "module @NAME@ (input logic clk_i);\n"
                              "    `DECLARE_TAG;\n"
                              "    `DECLARE_INNER;\n"
                              "endmodule\n");
    design.write_extra_header("inner.svh", "`define DECLARE_INNER logic inner_q\n");

    Analyzer analyzer;
    (void)design.index_ms(analyzer);

    // Every module, not one: which of them parse before the header is projected
    // depends on the worker count, so only checking all of them is certain to
    // cover a file that parsed after it.
    for (size_t i = 0; i < design.module_count(); ++i) {
        const auto values = shard_values(analyzer, design.module_uri(i));
        INFO("module " << design.module_uri(i));
        CHECK(values.contains("tag_q"));
        CHECK(values.contains("inner_q"));
    }
}

TEST_CASE("project shard: a file-scope parameter is indexed", "[index][scaling]") {
    // process_member() dispatches modules, packages, imports, classes and
    // typedefs.  A `parameter` / `localparam` written at compilation-unit scope
    // matched none of them, so a header made of them -- the shape of a central
    // params.svh -- contributed nothing to the project index.  It went unnoticed
    // because the file being edited resolves such a name through its own AST,
    // where the header's text is spliced in; only closed files went without.
    const auto dir = std::filesystem::temp_directory_path() / "lazyverilog-filescope-param";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const auto canonical_dir = std::filesystem::canonical(dir);
    const auto path = canonical_dir / "defs_pkg.sv";
    {
        std::ofstream out(path, std::ios::binary);
        out << "localparam int TOP_LEVEL_LP = 7;\n"
               "parameter int TOP_LEVEL_P = 8;\n";
    }

    Analyzer analyzer;
    analyzer.set_project_index_publish_debounce_ms(0);
    analyzer.set_project_config({}, {canonical_dir.string()}, {path.string()});
    analyzer.wait_for_background_index_idle();

    const auto values = shard_values(analyzer, uri_from_path(path));
    CHECK(values.contains("TOP_LEVEL_LP"));
    CHECK(values.contains("TOP_LEVEL_P"));

    std::error_code ec;
    std::filesystem::remove_all(canonical_dir, ec);
}
