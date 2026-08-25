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
