#include "analyzer.hpp"
#include "features/folding_range.hpp"
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <fstream>
#include <map>
#include <tuple>

static FoldingRangeRequestParams make_params(const std::string& uri) {
    FoldingRangeRequestParams p;
    p.textDocument.uri.raw_uri_ = uri;
    return p;
}

static bool has_fold(const std::vector<FoldingRange>& folds, int start, int end) {
    return std::any_of(folds.begin(), folds.end(), [&](const FoldingRange& r) {
        return r.startLine == start && r.endLine == end;
    });
}

static bool has_fold_kind(const std::vector<FoldingRange>& folds, int start, int end,
                          const std::string& kind) {
    return std::any_of(folds.begin(), folds.end(), [&](const FoldingRange& r) {
        return r.startLine == start && r.endLine == end && r.kind == kind;
    });
}

static const FoldingRange* find_fold_kind(const std::vector<FoldingRange>& folds, int start,
                                          int end, const std::string& kind) {
    auto it = std::find_if(folds.begin(), folds.end(), [&](const FoldingRange& r) {
        return r.startLine == start && r.endLine == end && r.kind == kind;
    });
    return it == folds.end() ? nullptr : &*it;
}

static bool has_fold_starting_at_and_ending_after(const std::vector<FoldingRange>& folds,
                                                  int start, int after) {
    return std::any_of(folds.begin(), folds.end(), [&](const FoldingRange& r) {
        return r.startLine == start && r.endLine > after;
    });
}

static bool has_exact_duplicate_fold(const std::vector<FoldingRange>& folds) {
    std::map<std::tuple<int, int, std::string>, int> seen;
    for (const auto& f : folds) {
        auto key = std::make_tuple(f.startLine, f.endLine, f.kind);
        if (++seen[key] > 1) return true;
    }
    return false;
}

// ── module body ───────────────────────────────────────────────────────────

TEST_CASE("foldingRange: module body folds", "[folding]") {
    Analyzer    analyzer;
    std::string uri = "file:///tmp/fold_module.sv";
    analyzer.open(uri, R"(module top(
    input logic a,
    output logic b
);
endmodule
)");
    auto folds = provide_folding_range(analyzer, make_params(uri));
    REQUIRE(!folds.empty());
    // module: line 0 (module top) to line 4 (endmodule)
    CHECK(has_fold(folds, 0, 4));
}

TEST_CASE("foldingRange: single-line module excluded", "[folding]") {
    Analyzer    analyzer;
    std::string uri = "file:///tmp/fold_single.sv";
    analyzer.open(uri, "module top; endmodule\n");
    auto folds = provide_folding_range(analyzer, make_params(uri));
    // module is on one line — no module fold
    for (const auto& f : folds)
        CHECK_FALSE((f.startLine == 0 && f.endLine == 0));
}

// ── nested begin/end ──────────────────────────────────────────────────────

TEST_CASE("foldingRange: nested begin/end fold independently", "[folding]") {
    Analyzer    analyzer;
    std::string uri = "file:///tmp/fold_begin.sv";
    analyzer.open(uri, R"(module top;
    always_ff @(posedge clk) begin
        if (en) begin
            a <= 1;
        end
    end
endmodule
)");
    auto folds = provide_folding_range(analyzer, make_params(uri));
    // outer begin/end: lines 1-5
    CHECK(has_fold(folds, 1, 5));
    // inner begin/end: lines 2-4
    CHECK(has_fold(folds, 2, 4));
    // module: lines 0-6
    CHECK(has_fold(folds, 0, 6));
}

// ── case statement ────────────────────────────────────────────────────────

TEST_CASE("foldingRange: case statement folds", "[folding]") {
    Analyzer    analyzer;
    std::string uri = "file:///tmp/fold_case.sv";
    analyzer.open(uri, R"(module top;
    always_comb begin
        case (sel)
            2'b00: a = 1;
            2'b01: begin
                a = 2;
                b = 3;
            end
            default: a = 0;
        endcase
    end
endmodule
)");
    auto folds = provide_folding_range(analyzer, make_params(uri));
    // case/endcase: lines 2-9
    CHECK(has_fold(folds, 2, 9));
    // multi-line case item (begin/end): lines 4-7
    CHECK(has_fold(folds, 4, 7));
}

// ── generate block ────────────────────────────────────────────────────────

TEST_CASE("foldingRange: generate block folds", "[folding]") {
    Analyzer    analyzer;
    std::string uri = "file:///tmp/fold_generate.sv";
    analyzer.open(uri, R"(module top;
    generate
        for (genvar i = 0; i < 4; i++) begin : g_loop
            assign out[i] = in[i];
        end
    endgenerate
endmodule
)");
    auto folds = provide_folding_range(analyzer, make_params(uri));
    // generate/endgenerate: lines 1-5
    CHECK(has_fold(folds, 1, 5));
    // loop generate block: lines 2-4
    CHECK(has_fold(folds, 2, 4));
}

// ── block comment ─────────────────────────────────────────────────────────

TEST_CASE("foldingRange: block comment folds", "[folding]") {
    Analyzer    analyzer;
    std::string uri = "file:///tmp/fold_block_comment.sv";
    analyzer.open(uri, R"(/* This is
   a multi-line
   block comment */
module top;
endmodule
)");
    auto folds = provide_folding_range(analyzer, make_params(uri));
    // block comment: lines 0-2, kind "comment"
    bool found = std::any_of(folds.begin(), folds.end(), [](const FoldingRange& r) {
        return r.startLine == 0 && r.endLine == 2 && r.kind == "comment";
    });
    CHECK(found);
}

// ── consecutive line comments ─────────────────────────────────────────────

TEST_CASE("foldingRange: consecutive line comments fold as one", "[folding]") {
    Analyzer    analyzer;
    std::string uri = "file:///tmp/fold_line_comments.sv";
    analyzer.open(uri, R"(// Line one
// Line two
// Line three
module top;
endmodule
)");
    auto folds = provide_folding_range(analyzer, make_params(uri));
    // three consecutive line comments → one fold [0,2] kind "comment"
    bool found = std::any_of(folds.begin(), folds.end(), [](const FoldingRange& r) {
        return r.startLine == 0 && r.endLine == 2 && r.kind == "comment";
    });
    CHECK(found);
}

// ── idempotency ───────────────────────────────────────────────────────────

TEST_CASE("foldingRange: idempotent", "[folding]") {
    Analyzer    analyzer;
    std::string uri = "file:///tmp/fold_idem.sv";
    analyzer.open(uri, R"(module top;
    always_ff @(posedge clk) begin
        a <= b;
    end
endmodule
)");
    auto p      = make_params(uri);
    auto first  = provide_folding_range(analyzer, p);
    auto second = provide_folding_range(analyzer, p);

    REQUIRE(first.size() == second.size());
    for (size_t i = 0; i < first.size(); ++i) {
        CHECK(first[i].startLine == second[i].startLine);
        CHECK(first[i].endLine   == second[i].endLine);
        CHECK(first[i].kind      == second[i].kind);
    }
}

// ── coverage gaps / regression tests ─────────────────────────────────────

TEST_CASE("foldingRange: trailing comments do not start comment folds", "[folding]") {
    Analyzer    analyzer;
    std::string uri = "file:///tmp/fold_trailing_comment.sv";
    analyzer.open(uri, R"(module top;
    assign a = b; // trailing comment belongs to code line
    // own-line comment after a trailing comment
endmodule
)");
    auto folds = provide_folding_range(analyzer, make_params(uri));

    // Pretty expected fold map:
    //   region  [0,3]  module top ... endmodule
    //
    // There must not be a comment fold [1,2], because that would fold line 1:
    //   assign a = b; // trailing comment belongs to code line
    // together with the own-line comment on line 2.
    CHECK(has_fold(folds, 0, 3));
    CHECK_FALSE(has_fold_kind(folds, 1, 2, "comment"));
}

TEST_CASE("foldingRange: preprocessor ifdef else endif folds branches", "[folding]") {
    Analyzer    analyzer;
    std::string uri = "file:///tmp/fold_ifdef_else.sv";
    analyzer.open(uri, R"(module top;
`ifdef USE_A
    assign y = a;
`else
    assign y = b;
`endif
endmodule
)");
    auto folds = provide_folding_range(analyzer, make_params(uri));

    // Pretty expected fold map:
    //   region  [0,6]  module top ... endmodule
    //   region  [1,2]  `ifdef USE_A branch
    //   region  [3,5]  `else branch, including closing `endif
    CHECK(has_fold(folds, 0, 6));
    CHECK(has_fold(folds, 1, 2));
    CHECK(has_fold(folds, 3, 5));
}

TEST_CASE("foldingRange: preprocessor folds coexist with local structural RTL folds",
          "[folding]") {
    Analyzer    analyzer;
    std::string uri = "file:///tmp/fold_ifdef_structural_priority.sv";
    analyzer.open(uri, R"(module top;
`ifdef USE_REGISTERED_VALID
always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
        valid_out <= 1'b0;
    end else begin
        valid_out <= pipe_vld[0];
    end
end
`else
always_comb begin
    valid_out = pipe_vld[0];
end
`endif
endmodule
)");
    auto folds = provide_folding_range(analyzer, make_params(uri));

    // Pretty expected fold map:
    //   region  [0,14]  module top ... endmodule
    //   region  [2,8]   always_ff begin ... end
    //   region  [3,5]   inactive-branch if begin/end block, recovered from tokens
    //   region  [9,13]  `else branch, including closing `endif
    //   region  [10,12] always_comb begin ... end
    //
    // Important UX regression:
    //   The `ifdef branch fold must exist so "za" on the directive line does
    //   not fall through to the whole module.  Local structural folds must also
    //   exist so inner always/if lines have nearby RTL folds available.
    CHECK(has_fold(folds, 0, 14));
    CHECK(has_fold(folds, 1, 8));
    CHECK(has_fold(folds, 2, 8));
    CHECK(has_fold(folds, 3, 5));
    CHECK(has_fold(folds, 9, 13));
    CHECK(has_fold(folds, 10, 12));
}

TEST_CASE("foldingRange: nested preprocessor conditionals fold independently", "[folding]") {
    Analyzer    analyzer;
    std::string uri = "file:///tmp/fold_nested_ifdef.sv";
    analyzer.open(uri, R"(module top;
`ifdef OUTER
    assign outer = 1'b1;
`ifdef INNER
    assign inner = 1'b1;
`endif
`endif
endmodule
)");
    auto folds = provide_folding_range(analyzer, make_params(uri));

    // Pretty expected fold map:
    //   region  [0,7]  module top ... endmodule
    //   region  [1,6]  `ifdef OUTER branch, including outer `endif
    //   region  [3,5]  `ifdef INNER branch, including inner `endif
    CHECK(has_fold(folds, 0, 7));
    CHECK(has_fold(folds, 1, 6));
    CHECK(has_fold(folds, 3, 5));
}

TEST_CASE("foldingRange: celldefine block folds", "[folding]") {
    Analyzer    analyzer;
    std::string uri = "file:///tmp/fold_celldefine.sv";
    analyzer.open(uri, R"(`celldefine
module cell_a;
endmodule
module cell_b;
endmodule
`endcelldefine
)");
    auto folds = provide_folding_range(analyzer, make_params(uri));

    // Pretty expected fold map:
    //   region  [0,5]  `celldefine ... `endcelldefine
    //   region  [1,2]  module cell_a ... endmodule
    //   region  [3,4]  module cell_b ... endmodule
    CHECK(has_fold(folds, 0, 5));
    CHECK(has_fold(folds, 1, 2));
    CHECK(has_fold(folds, 3, 4));
}

TEST_CASE("foldingRange: no exact duplicate folds", "[folding]") {
    Analyzer    analyzer;
    std::string uri = "file:///tmp/fold_no_duplicates.sv";
    analyzer.open(uri, R"(module top;
    always_comb begin
        case (sel)
            1'b0: begin
                a = 1'b0;
            end
            default: begin
                a = 1'b1;
            end
        endcase
    end
endmodule
)");
    auto folds = provide_folding_range(analyzer, make_params(uri));

    // Pretty expected property:
    //   each exact tuple (startLine, endLine, kind) appears at most once.
    CHECK_FALSE(has_exact_duplicate_fold(folds));
}

TEST_CASE("foldingRange: top-level package import run folds as imports", "[folding]") {
    Analyzer    analyzer;
    std::string uri = "file:///tmp/fold_imports.sv";
    analyzer.open(uri, R"(import pkg_a::*;
import pkg_b::item_b;
import pkg_c::*;

module top;
endmodule
)");
    auto folds = provide_folding_range(analyzer, make_params(uri));

    // Pretty expected fold map:
    //   imports [0,2]  three consecutive top-level package imports
    //   region  [4,5]  module top ... endmodule
    CHECK(has_fold_kind(folds, 0, 2, "imports"));
    CHECK(has_fold(folds, 4, 5));
}

TEST_CASE("foldingRange: multiline module port list folds", "[folding]") {
    Analyzer    analyzer;
    std::string uri = "file:///tmp/fold_port_list.sv";
    analyzer.open(uri, R"(module top(
    input  logic clk,
    input  logic rst_n,
    output logic done
);
endmodule
)");
    auto folds = provide_folding_range(analyzer, make_params(uri));

    // Pretty expected fold map:
    //   region  [0,4]  ANSI port list
    //   region  [0,5]  module top ... endmodule
    CHECK(has_fold(folds, 0, 4));
    CHECK(has_fold(folds, 0, 5));
}

TEST_CASE("foldingRange: function and task declarations fold", "[folding]") {
    Analyzer    analyzer;
    std::string uri = "file:///tmp/fold_function_task.sv";
    analyzer.open(uri, R"(module top;
    function automatic logic calc(
        input logic a,
        input logic b
    );
        calc = a & b;
    endfunction

    task automatic drive;
        done = 1'b1;
    endtask
endmodule
)");
    auto folds = provide_folding_range(analyzer, make_params(uri));

    // Pretty expected fold map:
    //   region  [0,11]  module top ... endmodule
    //   region  [1,6]   function automatic logic calc ... endfunction
    //   region  [8,10]  task automatic drive ... endtask
    CHECK(has_fold(folds, 0, 11));
    CHECK(has_fold(folds, 1, 6));
    CHECK(has_fold(folds, 8, 10));
}

TEST_CASE("foldingRange: class constraint and covergroup fold", "[folding]") {
    Analyzer    analyzer;
    std::string uri = "file:///tmp/fold_class_constraint_covergroup.sv";
    analyzer.open(uri, R"(class packet;
    rand bit [7:0] addr;

    constraint addr_c {
        addr inside {[8'h10:8'h1f]};
    }

    covergroup addr_cg;
        coverpoint addr {
            bins low = {[8'h00:8'h0f]};
        }
    endgroup
endclass
)");
    auto folds = provide_folding_range(analyzer, make_params(uri));

    // Pretty expected fold map:
    //   region  [0,12]  class packet ... endclass
    //   region  [3,5]   constraint addr_c { ... }
    //   region  [7,11]  covergroup addr_cg ... endgroup
    CHECK(has_fold(folds, 0, 12));
    CHECK(has_fold(folds, 3, 5));
    CHECK(has_fold(folds, 7, 11));
}

TEST_CASE("foldingRange: module-scoped import run folds as imports", "[folding]") {
    Analyzer    analyzer;
    std::string uri = "file:///tmp/fold_module_imports.sv";
    analyzer.open(uri, R"(module top;
    import pkg_a::*;
    import pkg_b::item_b;
    logic x;
endmodule
)");
    auto folds = provide_folding_range(analyzer, make_params(uri));

    // Pretty expected fold map:
    //   imports [1,2]  two consecutive module-scoped imports
    //   region  [0,4]  module top ... endmodule
    CHECK(has_fold_kind(folds, 1, 2, "imports"));
    CHECK(has_fold(folds, 0, 4));
}

TEST_CASE("foldingRange: single-line constructs are excluded", "[folding]") {
    Analyzer    analyzer;
    std::string uri = "file:///tmp/fold_single_line_constructs.sv";
    analyzer.open(uri, R"(module top;
    function logic pass(input logic a); pass = a; endfunction
    always_comb begin a = b; end
endmodule
)");
    auto folds = provide_folding_range(analyzer, make_params(uri));

    // Pretty expected fold map:
    //   region  [0,3]  module top ... endmodule
    //
    // Single-line function and begin/end constructs must not produce [1,1] or [2,2].
    CHECK(has_fold(folds, 0, 3));
    for (const auto& f : folds) {
        CHECK_FALSE((f.startLine == 1 && f.endLine == 1));
        CHECK_FALSE((f.startLine == 2 && f.endLine == 2));
    }
}

TEST_CASE("foldingRange: imports separated by comments are not one import run",
          "[folding]") {
    Analyzer    analyzer;
    std::string uri = "file:///tmp/fold_imports_comment_separator.sv";
    analyzer.open(uri, R"(import pkg_a::*;

// This comment documents why the next import is conditional or unusual.
import pkg_b::*;

module top;
endmodule
)");
    auto folds = provide_folding_range(analyzer, make_params(uri));

    // Pretty expected fold map:
    //   region [5,6] module top ... endmodule
    //
    // The two imports are not a visually consecutive import group: a blank line
    // and an own-line comment separate them.  A single imports fold [0,3] would
    // hide the separator comment and make two unrelated import groups look like
    // one block.
    CHECK_FALSE(has_fold_kind(folds, 0, 3, "imports"));
}

TEST_CASE("foldingRange: inactive branch recovers case folds from disabled tokens",
          "[folding]") {
    Analyzer    analyzer;
    std::string uri = "file:///tmp/fold_inactive_case.sv";
    analyzer.open(uri, R"(module top;
`ifdef NEVER_DEFINED_FOR_THIS_TEST
always_comb begin
    case (sel)
        1'b0: begin
            a = 1'b0;
        end
    endcase
end
`endif
endmodule
)");
    auto folds = provide_folding_range(analyzer, make_params(uri));

    // Pretty expected fold map:
    //   region [1,9] `ifdef NEVER_DEFINED_FOR_THIS_TEST ... `endif
    //   region [2,8] inactive always_comb begin ... end, recovered from tokens
    //   region [3,7] inactive case ... endcase, recovered from tokens
    //   region [4,6] inactive case-item begin ... end, recovered from tokens
    //
    // Active code gets a CaseStatementSyntax fold.  Disabled code should expose
    // an equivalent local fold so navigation does not degrade merely because the
    // macro is currently undefined.
    CHECK(has_fold(folds, 1, 9));
    CHECK(has_fold(folds, 2, 8));
    CHECK(has_fold(folds, 3, 7));
    CHECK(has_fold(folds, 4, 6));
}

TEST_CASE("foldingRange: nested conditionals inside inactive branches fold independently",
          "[folding]") {
    Analyzer    analyzer;
    std::string uri = "file:///tmp/fold_nested_inactive_ifdef.sv";
    analyzer.open(uri, R"(module top;
`ifdef OUTER_DISABLED
    `ifdef INNER_DISABLED
        assign inner = 1'b1;
    `endif
`endif
endmodule
)");
    auto folds = provide_folding_range(analyzer, make_params(uri));

    // This guards the claim that nested directives inside disabled branches are
    // still surfaced by slang as directive trivia and therefore do not require
    // manual raw-source scanning.  If this fails, the inactive-token path must
    // learn how to process directive tokens explicitly.
    CHECK(has_fold(folds, 1, 5));
    CHECK(has_fold(folds, 2, 4));
}

TEST_CASE("foldingRange: inactive branch recovers keyword-delimited regions",
          "[folding]") {
    Analyzer    analyzer;
    std::string uri = "file:///tmp/fold_inactive_keyword_regions.sv";
    analyzer.open(uri, R"(module top;
`ifdef DISABLED_REGIONS
module inactive_mod;
endmodule
interface inactive_if;
endinterface
program inactive_prog;
endprogram
package inactive_pkg;
endpackage
class inactive_class;
    function void inactive_func;
    endfunction
    task inactive_task;
    endtask
endclass
checker inactive_checker;
endchecker
primitive inactive_udp(out, in);
endprimitive
config inactive_cfg;
endconfig
specify
endspecify
generate
endgenerate
property inactive_prop;
endproperty
sequence inactive_seq;
endsequence
`endif
endmodule
)");
    auto folds = provide_folding_range(analyzer, make_params(uri));

    // Inactive code is not represented by the normal AST, so these folds must be
    // recovered from slang disabled token kinds rather than raw source strings.
    CHECK(has_fold(folds, 2, 3));   // module ... endmodule
    CHECK(has_fold(folds, 4, 5));   // interface ... endinterface
    CHECK(has_fold(folds, 6, 7));   // program ... endprogram
    CHECK(has_fold(folds, 8, 9));   // package ... endpackage
    CHECK(has_fold(folds, 10, 15)); // class ... endclass
    CHECK(has_fold(folds, 11, 12)); // function ... endfunction
    CHECK(has_fold(folds, 13, 14)); // task ... endtask
    CHECK(has_fold(folds, 16, 17)); // checker ... endchecker
    CHECK(has_fold(folds, 18, 19)); // primitive ... endprimitive
    CHECK(has_fold(folds, 20, 21)); // config ... endconfig
    CHECK(has_fold(folds, 22, 23)); // specify ... endspecify
    CHECK(has_fold(folds, 24, 25)); // generate ... endgenerate
    CHECK(has_fold(folds, 26, 27)); // property ... endproperty
    CHECK(has_fold(folds, 28, 29)); // sequence ... endsequence
}

TEST_CASE("foldingRange: inactive branch recovers fork join variants",
          "[folding]") {
    Analyzer    analyzer;
    std::string uri = "file:///tmp/fold_inactive_fork_regions.sv";
    analyzer.open(uri, R"(module top;
`ifdef DISABLED_FORKS
fork
join
fork
join_any
fork
join_none
`endif
endmodule
)");
    auto folds = provide_folding_range(analyzer, make_params(uri));

    CHECK(has_fold(folds, 2, 3)); // fork ... join
    CHECK(has_fold(folds, 4, 5)); // fork ... join_any
    CHECK(has_fold(folds, 6, 7)); // fork ... join_none
}

TEST_CASE("foldingRange: inactive branch recovers brace-delimited coverage regions",
          "[folding]") {
    Analyzer    analyzer;
    std::string uri = "file:///tmp/fold_inactive_brace_regions.sv";
    analyzer.open(uri, R"(class active_wrapper;
`ifdef DISABLED_COVERAGE
constraint inactive_c {
    a inside {[0:3]};
}
covergroup inactive_cg;
    coverpoint a {
        bins low = {
            [0:3]
        };
        illegal_bins bad = {
            4
        };
    }
    cross a, b {
        ignore_bins selected = {
            binsof(a)
        };
    }
endgroup
`endif
endclass
)");
    auto folds = provide_folding_range(analyzer, make_params(uri));

    // The constraint / coverpoint / cross / bins folds are recovered with token
    // kinds and brace-depth tracking.  Inner set-expression braces must not close
    // the outer constraint fold early.
    CHECK(has_fold(folds, 2, 4));   // constraint ... matching }
    CHECK(has_fold(folds, 6, 13));  // coverpoint ... matching }
    CHECK(has_fold(folds, 7, 9));   // bins ... matching }
    CHECK(has_fold(folds, 10, 12)); // illegal_bins ... matching }
    CHECK(has_fold(folds, 14, 18)); // cross ... matching }
    CHECK(has_fold(folds, 15, 17)); // ignore_bins ... matching }
}

TEST_CASE("foldingRange: emitted character offsets describe real line columns",
          "[folding]") {
    Analyzer    analyzer;
    std::string uri = "file:///tmp/fold_characters.sv";
    analyzer.open(uri, R"(module top;
    always_comb begin
        a = b;
    end
endmodule
)");
    auto folds = provide_folding_range(analyzer, make_params(uri));

    const FoldingRange* module = find_fold_kind(folds, 0, 4, "region");
    REQUIRE(module != nullptr);
    CHECK(module->startCharacter == 0);
    CHECK(module->endCharacter == 9); // strlen("endmodule")

    const FoldingRange* always = find_fold_kind(folds, 1, 3, "region");
    REQUIRE(always != nullptr);
    CHECK(always->startCharacter == 4); // indentation before always_comb
    CHECK(always->endCharacter == 7);   // four spaces + strlen("end")
}

// ── begin/end control keyword attribution ────────────────────────────────────

TEST_CASE("foldingRange: begin/end fold starts at control keyword when begin is on next line",
          "[folding]") {
    Analyzer    analyzer;
    std::string uri = "file:///tmp/fold_begin_attribution.sv";
    analyzer.open(uri, R"(module top;
    always_comb
    begin
        a = b;
    end
endmodule
)");
    auto folds = provide_folding_range(analyzer, make_params(uri));

    // The fold must start at line 1 (always_comb), not line 2 (begin).
    // Line 4 (end) closes it.
    CHECK(has_fold(folds, 1, 4));
    // No fold starting at the bare begin line
    for (const auto& f : folds)
        CHECK_FALSE((f.startLine == 2 && f.endLine == 4));
}

// ── active fork/join variants ─────────────────────────────────────────────────

TEST_CASE("foldingRange: active fork/join variants fold", "[folding]") {
    Analyzer    analyzer;
    std::string uri = "file:///tmp/fold_fork_join.sv";
    analyzer.open(uri, R"(module top;
    initial begin
        fork
            task_a();
        join
        fork
            task_b();
        join_any
        fork
            task_c();
        join_none
    end
endmodule
)");
    auto folds = provide_folding_range(analyzer, make_params(uri));

    CHECK(has_fold(folds, 2, 4));  // fork ... join
    CHECK(has_fold(folds, 5, 7));  // fork ... join_any
    CHECK(has_fold(folds, 8, 10)); // fork ... join_none
}

// ── clocking block ────────────────────────────────────────────────────────────

TEST_CASE("foldingRange: clocking/endclocking fold", "[folding]") {
    Analyzer    analyzer;
    std::string uri = "file:///tmp/fold_clocking.sv";
    analyzer.open(uri, R"(module top(input logic clk);
    clocking cb @(posedge clk);
        input  data_in;
        output data_out;
    endclocking
endmodule
)");
    auto folds = provide_folding_range(analyzer, make_params(uri));

    // clocking block: lines 1-4
    CHECK(has_fold(folds, 1, 4));
    // module: lines 0-5
    CHECK(has_fold(folds, 0, 5));
}

// ── import run excludes DPI imports ──────────────────────────────────────────

TEST_CASE("foldingRange: import run excludes DPI imports", "[folding]") {
    Analyzer    analyzer;
    std::string uri = "file:///tmp/fold_dpi_import.sv";
    analyzer.open(uri, R"(import pkg_a::*;
import pkg_b::*;
import "DPI-C" function void c_func(int x);
import pkg_c::*;
module top;
endmodule
)");
    auto folds = provide_folding_range(analyzer, make_params(uri));

    // pkg_a + pkg_b form one import run [0,1]
    CHECK(has_fold_kind(folds, 0, 1, "imports"));
    // DPI import breaks the run; pkg_c is alone on line 3 — no run fold for it
    CHECK_FALSE(has_fold_kind(folds, 2, 3, "imports"));
    CHECK_FALSE(has_fold_kind(folds, 0, 3, "imports"));
}

// ── typedef enum / struct / union ────────────────────────────────────────────

TEST_CASE("foldingRange: typedef enum body folds", "[folding]") {
    Analyzer    analyzer;
    std::string uri = "file:///tmp/fold_typedef_enum.sv";
    analyzer.open(uri, R"(package types_pkg;
    typedef enum logic [1:0] {
        STATE_IDLE  = 2'b00,
        STATE_BUSY  = 2'b01,
        STATE_DONE  = 2'b10
    } state_t;
endpackage
)");
    auto folds = provide_folding_range(analyzer, make_params(uri));

    // typedef enum body: line 1 (typedef) to line 5 (closing })
    CHECK(has_fold(folds, 1, 5));
    // package: lines 0-6
    CHECK(has_fold(folds, 0, 6));
}

TEST_CASE("foldingRange: typedef struct body folds", "[folding]") {
    Analyzer    analyzer;
    std::string uri = "file:///tmp/fold_typedef_struct.sv";
    analyzer.open(uri, R"(package types_pkg;
    typedef struct packed {
        logic        valid;
        logic [7:0]  data;
        logic [1:0]  keep;
    } axi_word_t;
endpackage
)");
    auto folds = provide_folding_range(analyzer, make_params(uri));

    // typedef struct body: line 1 (typedef) to line 5 (closing })
    CHECK(has_fold(folds, 1, 5));
}

TEST_CASE("foldingRange: typedef union body folds", "[folding]") {
    Analyzer    analyzer;
    std::string uri = "file:///tmp/fold_typedef_union.sv";
    analyzer.open(uri, R"(package types_pkg;
    typedef union packed {
        logic [31:0] raw;
        struct packed { logic [15:0] hi; logic [15:0] lo; } halves;
    } word_u;
endpackage
)");
    auto folds = provide_folding_range(analyzer, make_params(uri));

    // typedef union body: line 1 (typedef) to line 4 (closing })
    CHECK(has_fold(folds, 1, 4));
}

TEST_CASE("foldingRange: enum without typedef folds", "[folding]") {
    Analyzer    analyzer;
    std::string uri = "file:///tmp/fold_enum_no_typedef.sv";
    analyzer.open(uri, R"(module top;
    enum logic [1:0] {
        A = 2'b00,
        B = 2'b01,
        C = 2'b10
    } state;
endmodule
)");
    auto folds = provide_folding_range(analyzer, make_params(uri));

    // enum body: line 1 (enum) to line 5 (closing })
    CHECK(has_fold(folds, 1, 5));
}

// ── #ifndef inactive branch ───────────────────────────────────────────────────

TEST_CASE("foldingRange: #ifndef inactive branch folds", "[folding]") {
    Analyzer    analyzer;
    std::string uri = "file:///tmp/fold_ifndef.sv";
    // ALWAYS_DEFINED is not defined, so the body is the inactive branch.
    analyzer.open(uri, R"(module top;
`ifndef ALWAYS_DEFINED
    logic unused_a;
    logic unused_b;
`endif
endmodule
)");
    auto folds = provide_folding_range(analyzer, make_params(uri));

    // Preprocessor fold covers lines 1-4 (`ifndef ... `endif)
    CHECK(has_fold(folds, 1, 4));
    // Module still folds
    CHECK(has_fold(folds, 0, 5));
}

// ── module headers and declaration runs ──────────────────────────────────────

TEST_CASE("foldingRange: parameterized module folds parameter list port list and declarations",
          "[folding]") {
    Analyzer    analyzer;
    std::string uri = "file:///tmp/fold_parameterized_header_and_decls.sv";
    analyzer.open(uri, R"(module folding_demo #(
    parameter int WIDTH = 8,
    parameter int DEPTH = 16,
    parameter int STAGES = 3
)(
    input     logic                   clk,
    input     logic                   rst_n,
    input     logic [WIDTH-1:0]       data_in,
    input     logic                   valid_in,
    output    logic [WIDTH-1:0]       data_out,
    output    logic                   valid_out
);

logic               [WIDTH-1:0]         pipe_data[STAGES]                   ;
logic                                   pipe_vld[STAGES]                    ;
logic               [WIDTH-1:0]         selected_data                       ;
endmodule
)");
    auto folds = provide_folding_range(analyzer, make_params(uri));

    // Pretty expected fold map for the user-facing example:
    //   region [0,3]   module parameter port list "#(...)", excluding the
    //                  shared ")(" delimiter line for Neovim's line fold model.
    //   region [5,10]  ANSI port list "(...)", excluding both delimiter lines.
    //   region [13,15] consecutive module-scoped declarations
    //   region [0,16]  whole module fold, including the header.
    //
    // LSP can describe exact column-delimited folds, but Neovim's built-in
    // foldexpr merges adjacent line ranges like [0,4] + [4,11] + [11,16].
    // These intentionally non-touching header child folds avoid making the
    // header lists one continuous line fold, while the enclosing module range
    // still covers the whole module so folding from the body closes the module
    // including its header.
    CHECK(has_fold(folds, 0, 3));
    CHECK(has_fold(folds, 5, 10));
    CHECK_FALSE(has_fold(folds, 0, 11));
    CHECK(has_fold(folds, 13, 15));
    CHECK(has_fold(folds, 0, 16));
}

TEST_CASE("foldingRange: varied semicolon declarations fold as one consecutive run",
          "[folding]") {
    Analyzer    analyzer;
    std::string uri = "file:///tmp/fold_varied_declaration_run.sv";
    analyzer.open(uri, R"(module top;
    localparam int WIDTH = 8;
    wire [WIDTH-1:0] data_w;
    var logic        explicit_v;
    reg              data_q;
    integer          loop_i;
    time             last_seen;
    supply0          tie_low;
endmodule
)");
    auto folds = provide_folding_range(analyzer, make_params(uri));

    // A declaration run should include parameter, net, variable, integer/time,
    // explicit "var", and supply net declarations instead of only "logic"
    // declarations.
    CHECK(has_fold(folds, 1, 7));
}

TEST_CASE("foldingRange: declaration runs stop at non-declaration statements",
          "[folding]") {
    Analyzer    analyzer;
    std::string uri = "file:///tmp/fold_declaration_run_break.sv";
    analyzer.open(uri, R"(module top;
    logic a;
    logic b;
    assign b = a;
    logic c;
    logic d;
endmodule
)");
    auto folds = provide_folding_range(analyzer, make_params(uri));

    // The assign statement is deliberately not folded into the declaration
    // range, and it breaks the two declaration groups into independent folds.
    CHECK(has_fold(folds, 1, 2));
    CHECK(has_fold(folds, 4, 5));
    CHECK_FALSE(has_fold(folds, 1, 5));
}

TEST_CASE("foldingRange: non-ANSI port declarations join declaration runs",
          "[folding]") {
    Analyzer    analyzer;
    std::string uri = "file:///tmp/fold_non_ansi_port_declarations.sv";
    analyzer.open(uri, R"(module top (
    clk,
    rst_n,
    data_in,
    data_out
);
    input  logic       clk;
    input  logic       rst_n;
    input  payload_t   data_in;
    output payload_t   data_out;
    logic              valid_q;
    logic [3:0]        count_q;
endmodule
)");
    auto folds = provide_folding_range(analyzer, make_params(uri));

    // Non-ANSI ports are declared as semicolon-terminated declarations after
    // the header.  They should fold as one consecutive declaration run together
    // with following ordinary variables, including user-defined port types.
    CHECK(has_fold_kind(folds, 6, 11, "declarations"));
}

TEST_CASE("foldingRange: user-defined type declarations join declaration runs",
          "[folding]") {
    Analyzer    analyzer;
    std::string uri = "file:///tmp/fold_user_type_declarations.sv";
    analyzer.open(uri, R"(package types_pkg;
    typedef struct packed {
        logic valid;
        logic [7:0] data;
    } payload_t;
endpackage

module top;
    typedef enum logic [1:0] {
        IDLE,
        BUSY
    } state_e;

    state_e              state_q;
    types_pkg::payload_t payload_q;
    logic                valid_q;
endmodule
)");
    auto folds = provide_folding_range(analyzer, make_params(uri));

    // The final declaration run must include the two identifier-led
    // user-defined declarations as well as the keyword-led logic declaration.
    // Lexically guessing "state_e state_q;" would be unsafe because a similar
    // token sequence can be an instantiation; the implementation uses AST
    // declaration nodes for this active-code case.
    CHECK(has_fold_kind(folds, 13, 15, "declarations"));
}

TEST_CASE("foldingRange: AST folds from included files are not emitted for current document",
          "[folding]") {
    const std::string include_path = "/tmp/lazyverilog_folding_include.svh";
    {
        std::ofstream out(include_path);
        out << R"(module included_fold_target #(
    parameter int W = 8
)(
    input logic clk
);
    included_type_t from_include;
    included_type_t also_from_include;
endmodule
)";
    }

    Analyzer    analyzer;
    std::string uri = "file:///tmp/fold_include_owner.sv";
    std::string text = R"(`include "lazyverilog_folding_include.svh"

module current_top;
    logic a;
    logic b;
endmodule
)";
    analyzer.open(uri, text);
    auto folds = provide_folding_range(analyzer, make_params(uri));

    const int current_line_count = 6;
    for (const auto& fold : folds) {
        CHECK(fold.startLine >= 0);
        CHECK(fold.endLine < current_line_count);
    }

    // The current file's own declaration run remains available, but declaration
    // and header folds from the included file must not be reported in this
    // document's coordinates.
    CHECK(has_fold_kind(folds, 3, 4, "declarations"));
    CHECK_FALSE(has_fold(folds, 0, 4));
}
