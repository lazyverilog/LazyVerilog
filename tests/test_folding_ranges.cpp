#include "analyzer.hpp"
#include "features/folding_range.hpp"
#include "string_utils.hpp"
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

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

static bool same_folds(const std::vector<FoldingRange>& a, const std::vector<FoldingRange>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].startLine != b[i].startLine || a[i].endLine != b[i].endLine ||
            a[i].startCharacter != b[i].startCharacter ||
            a[i].endCharacter != b[i].endCharacter || a[i].kind != b[i].kind)
            return false;
    }
    return true;
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

TEST_CASE("foldingRange: keyword regions do not cross preprocessor branch boundaries",
          "[folding]") {
    Analyzer    analyzer;
    std::string uri = "file:///tmp/fold_no_cross_branch_task.sv";
    analyzer.open(uri, R"(`ifdef FOO
    task print_data();
        $display("%0d", data);
    endtask
    task req_data();
`elsif BAR
        $display("%0d", data);
    endtask
`endif
)");
    auto folds = provide_folding_range(analyzer, make_params(uri));

    // Expected fold map:
    //   region [0,4]  `ifdef FOO branch, ending before `elsif
    //   region [1,3]  complete task print_data inside the FOO branch
    //   region [5,8]  `elsif BAR branch, including closing `endif
    //
    // Important regression:
    //   Do not fold task req_data from [4,7].  Its task header is in the FOO
    //   branch, but the endtask is in the BAR branch.  Those tokens are never
    //   simultaneously active after preprocessing, so pairing them would expose
    //   a misleading structural fold across mutually exclusive branches.
    CHECK(has_fold(folds, 0, 4));
    CHECK(has_fold(folds, 1, 3));
    CHECK(has_fold(folds, 5, 8));
    CHECK_FALSE(has_fold(folds, 4, 7));
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

    // Expected fold map for the user-facing example:
    //   region [0,4]   module parameter port list "#(...)", paren to paren.
    //   region [4,11]  ANSI port list "(...)", paren to paren.
    //   region [13,15] consecutive module-scoped declarations
    //   region [0,16]  whole module fold, including the header.
    //
    // The two header lists are matched paren to paren by the token scan, so
    // they share the ")(" line.  Recognizing a module header as such -- and
    // trimming the shared delimiter line out, which is what Neovim's line fold
    // model wants -- needs ModuleHeaderSyntax, and folds are derived from the
    // token scan alone.  Neovim's foldexpr therefore merges these two into one
    // continuous header fold rather than offering them separately.
    CHECK(has_fold(folds, 0, 4));
    CHECK(has_fold(folds, 4, 11));
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
    // the header.  Joining them to the following variables into one run [6,11]
    // needs PortDeclarationSyntax: "input payload_t data_in;" is identifier-led
    // once the direction keyword is consumed, and the token scan cannot tell it
    // from an instantiation.  So the run starts where the keyword-led
    // declarations do.
    CHECK(has_fold_kind(folds, 10, 11, "declarations"));
    CHECK_FALSE(has_fold_kind(folds, 6, 11, "declarations"));
}

TEST_CASE("foldingRange: identifier-led declarations do not fold without the AST",
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

    // "state_e state_q;" and "my_child u_inst (...);" are the same token shape,
    // so a lexical guess would fold instantiations as variables.  The token
    // scan declines to guess, which means this run -- two identifier-led
    // declarations and one keyword-led one -- produces no fold at all.  Telling
    // the two apart needs DataDeclarationSyntax from the parsed tree, and folds
    // are derived from the token scan alone.
    CHECK_FALSE(has_fold_kind(folds, 13, 15, "declarations"));
    CHECK_FALSE(std::any_of(folds.begin(), folds.end(), [](const FoldingRange& r) {
        return r.startLine == 13;
    }));
}

TEST_CASE("foldingRange: parameterized instances fold only their parameter list",
          "[folding]") {
    Analyzer    analyzer;
    std::string uri = "file:///tmp/fold_parameterized_instance.sv";
    analyzer.open(uri, R"(module top;
    memory #(
        .WIDTH(DATA_W),
        .DEPTH(16)
    ) u_mem (
        .clk_i(clk),
        .rst_ni(rst_n),
        .data_o(data)
    );
endmodule
)");
    auto folds = provide_folding_range(analyzer, make_params(uri));

    // Recognizing the whole statement [1,8] as one instance needs
    // HierarchyInstantiationSyntax, and folds are derived from the token scan
    // alone.  What the paren matcher produces instead is the "#(...)" override
    // block on its own, ending on the ") u_mem (" line.
    //
    // Vim's line-based fold model marks one fold start per line, so this
    // shorter range is the fold za/zc reaches first: closing a fold on the
    // instance line hides the parameter overrides and leaves the port
    // connection list open.  The instance never collapses as a whole.
    CHECK(has_fold(folds, 1, 4));
    CHECK_FALSE(has_fold_kind(folds, 1, 8, "instance"));
    CHECK_FALSE(std::any_of(folds.begin(), folds.end(),
                            [](const FoldingRange& r) { return r.kind == "instance"; }));
}

TEST_CASE("foldingRange: folds from included files are not emitted for current document",
          "[folding]") {
    const std::string include_path =
        (std::filesystem::temp_directory_path() / "lazyverilog_folding_include.svh").string();
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

// ── Cost model ────────────────────────────────────────────────────────────
//
// The editor asks for folds from the `didChange` notification itself, so every
// keystroke pays for one whole-file fold computation.  Two steps here used to
// grow with the square of the file:
//
//   * emit() rebuilt a line table over the whole buffer for each fold it
//     produced, making N folds over an M-byte file cost O(N x M);
//   * normalize_folds() answered its grouping questions by scanning the fold
//     list, once per fold -- and one of those scans was nested two deep.
//
// Both are invisible on the small files the cases above use and dominate on a
// real RTL block: a 13k-line file spent ~170 ms per request, which an editor
// then serialized ahead of the completion the user was waiting for.
//
// Guard it as a ratio between two inputs of the same shape and different size,
// never a millisecond budget: a doubled file should cost about twice as much,
// and anything quadratic costs four times.

namespace {

/// @p stages structurally identical blocks: each one a state enum, a case
/// statement, nested if/else, a loop, an always_ff and a function -- the fold
/// shapes an RTL file is made of.
static std::string folding_scaling_source(int stages) {
    std::string text = "module scaling_block (input logic clk_i, input logic rst_ni);\n";
    for (int i = 0; i < stages; ++i) {
        const std::string n = std::to_string(i);
        text += "  // ---- stage " + n + " ----\n";
        text += "  typedef enum logic [1:0] {\n    S" + n + "_IDLE,\n    S" + n +
                "_RUN,\n    S" + n + "_DONE\n  } state" + n + "_e;\n";
        text += "  state" + n + "_e state" + n + "_q, state" + n + "_d;\n";
        text += "  logic [31:0] stage" + n + "_q, stage" + n + "_d;\n";
        text += "  always_comb begin\n    unique case (state" + n + "_q)\n";
        text += "      S" + n + "_IDLE: begin\n        if (stage" + n +
                "_q != '0) begin\n          state" + n + "_d = S" + n +
                "_RUN;\n        end else begin\n          state" + n + "_d = S" + n +
                "_IDLE;\n        end\n      end\n";
        text += "      S" + n + "_RUN: begin\n        for (int j = 0; j < 8; j++) begin\n"
                "          stage" + n + "_d[j] = ~stage" + n + "_q[j];\n        end\n"
                "        state" + n + "_d = S" + n + "_DONE;\n      end\n";
        text += "      default: state" + n + "_d = S" + n + "_IDLE;\n    endcase\n  end\n";
        text += "  always_ff @(posedge clk_i or negedge rst_ni) begin\n"
                "    if (!rst_ni) begin\n      state" + n + "_q <= S" + n + "_IDLE;\n"
                "      stage" + n + "_q <= '0;\n    end else begin\n      state" + n +
                "_q <= state" + n + "_d;\n      stage" + n + "_q <= stage" + n +
                "_d;\n    end\n  end\n";
        text += "  function automatic logic [31:0] mix" + n + "(input logic [31:0] a);\n"
                "    return a ^ 32'd" + n + ";\n  endfunction\n";
    }
    text += "endmodule\n";
    return text;
}

/// Fastest of @p runs, for the reason test_shared_header_scaling.cpp takes the
/// minimum: the cost being guarded is a fixed amount of extra work, so it raises
/// the floor, and everything a shared runner adds only ever makes a sample
/// slower.
///
/// **Every run needs its own document snapshot.**  `provide_folding_range()`
/// caches its answer on the immutable `DocumentState` it computed it from, so a
/// second request against the same snapshot returns a copy of the first one's
/// vector.  Sampling one document therefore measures one computation and N-1
/// copies, and the *minimum* of that set is always a copy: measured 0.0087 ms
/// against 7.21 ms for the same input, an 831x understatement that no threshold
/// on the ratio can detect, because the ratio of two vector copies is linear by
/// construction whatever folding itself does.
///
/// This is the trap docs/dev/indexing.md describes for the shard cache -- "a
/// warm test has to distinguish a hit from a reparse" -- in the folding tests.
/// A fresh `Analyzer` and a run-unique URI are what keep every sample a real
/// computation.  `Analyzer::open()` parses synchronously and returns before the
/// timed region starts, so it is not charged to the measurement.
static double fastest_folding_ms(int stages, int runs, size_t& folds_out) {
    const std::string source = folding_scaling_source(stages);

    std::vector<double> samples;
    samples.reserve((size_t)runs);
    for (int i = 0; i < runs; ++i) {
        Analyzer          analyzer;
        const std::string uri = "file:///fold_scaling_" + std::to_string(stages) + "_" +
                                std::to_string(i) + ".sv";
        analyzer.open(uri, source);

        const auto start = std::chrono::steady_clock::now();
        auto       folds = provide_folding_range(analyzer, make_params(uri));
        samples.push_back(
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                .count());
        folds_out = folds.size();
    }
    std::sort(samples.begin(), samples.end());
    return samples.front();
}

} // namespace

TEST_CASE("foldingRange: cost grows with the file, not with its square",
          "[folding][scaling]") {
    constexpr int kStages = 240;
    constexpr int kRuns   = 5;

    size_t small_folds = 0;
    size_t large_folds = 0;

    // Warm the allocator and instruction cache so the first measured half is not
    // charged for both.
    (void)fastest_folding_ms(kStages, 2, small_folds);

    const double small_ms = fastest_folding_ms(kStages, kRuns, small_folds);
    const double large_ms = fastest_folding_ms(kStages * 2, kRuns, large_folds);

    // Like for like: the large input must really be twice the small one.
    REQUIRE(small_folds > 0);
    // The module's own fold is the one that is not per stage.
    CHECK(large_folds - 1 == (small_folds - 1) * 2);

    const double ratio = large_ms / small_ms;
    std::cout << "\n[folding scaling] stages=" << kStages << " folds=" << small_folds
              << " ms=" << small_ms << "  stages=" << (kStages * 2)
              << " folds=" << large_folds << " ms=" << large_ms << " ratio=" << ratio << "\n";

    // Linear is 2.0 and quadratic is 4.0.  Measured at 2.2-2.5 across a 16x
    // range of sizes, against 3.5 for the quadratic passes this replaced, which
    // this threshold fails.
    CHECK(ratio < 3.0);
}

TEST_CASE("foldingRange: cost stays linear at large fold counts", "[folding][scaling]") {
    // The guard above compares 3k folds against 6k, and a quadratic term can hide
    // under a 3.0 threshold at that size: the header-partner pass measured 2.5 /
    // 2.9 / 2.8 across the first three doublings and only broke out on the fourth,
    // at 5.86x.  A file that large is not hypothetical -- a generated register
    // block reaches it -- and 903 ms landed on the one thread that answers
    // completions, once per keystroke.
    //
    // So take the doubling where the old shape actually showed itself.  Two runs
    // per side: at ~80 ms a sample this is still well under a second, and the
    // cost being guarded raises the floor, so the minimum is the honest statistic.
    constexpr int kStages = 1920;
    constexpr int kRuns   = 2;

    size_t small_folds = 0;
    size_t large_folds = 0;

    const double small_ms = fastest_folding_ms(kStages, kRuns, small_folds);
    const double large_ms = fastest_folding_ms(kStages * 2, kRuns, large_folds);

    REQUIRE(small_folds > 20000);
    CHECK(large_folds - 1 == (small_folds - 1) * 2);

    const double ratio = large_ms / small_ms;
    std::cout << "\n[folding scaling, large] folds=" << small_folds << " ms=" << small_ms
              << "  folds=" << large_folds << " ms=" << large_ms << " ratio=" << ratio << "\n";

    // Measured 1.7-1.8 once the partner pass stopped scanning the whole fold
    // list; the version this replaced measures 5.86 and fails.
    CHECK(ratio < 3.0);
}

TEST_CASE("foldingRange: the scaling guard measures a computation, not a cache hit",
          "[folding][scaling]") {
    // The guard above is only meaningful while `fastest_folding_ms()` samples
    // real computations.  A per-snapshot cache landed between that helper being
    // written and this one, and turned every sample after the first into a
    // vector copy -- so the guard reported 0.0087 ms for an input that costs
    // 7.2 ms, and could no longer fail for any reason.
    //
    // Pin both halves of that story: the cache must serve a repeat, and the
    // helper must not be measuring it.
    constexpr int kStages = 240;

    Analyzer          analyzer;
    const std::string uri = "file:///fold_cache_probe.sv";
    analyzer.open(uri, folding_scaling_source(kStages));

    const auto time_one = [&] {
        const auto start = std::chrono::steady_clock::now();
        auto       folds = provide_folding_range(analyzer, make_params(uri));
        REQUIRE(folds.size() > 0);
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
            .count();
    };

    (void)time_one(); // populates the snapshot's cache
    double cached_ms = time_one();
    for (int i = 0; i < 4; ++i)
        cached_ms = std::min(cached_ms, time_one());

    size_t       folds       = 0;
    const double computed_ms = fastest_folding_ms(kStages, 3, folds);

    std::cout << "\n[folding cache] computed=" << computed_ms << " ms  cached repeat=" << cached_ms
              << " ms\n";

    // Measured ~7.2 ms against ~0.009 ms, so this has three orders of magnitude
    // of headroom; it fires only if the helper starts sampling cached answers.
    CHECK(computed_ms > cached_ms * 10.0);
}

// ── Folds while a parse is in flight ──────────────────────────────────────
//
// An editor asks for folds from the `didOpen`/`didChange` notification itself,
// so its request reliably arrives while the parse that notification started is
// still running and the snapshot carries text but no syntax tree.  Answering
// "no folds" there is not harmless: Neovim applies the empty set to the whole
// buffer and asks again only on the next change, so the file is left unfoldable
// until the user types -- and typing lands in the same window again.
//
// Folds are derived from the token scan alone, so the syntax tree cannot change
// the answer.  These two tests pin that: the reply for a given text is the same
// whether or not its parse has landed.  Both hold the parse worker with
// set_parse_paused() so the window is a state they enter rather than one they
// race the worker for -- retrying until the worker happened to lose is not a
// race a test can win on a one-CPU slice, where the scheduler that runs the
// worker first runs it first on every attempt.

// Block until `uri` has a parsed tree again.  Bounded so a wedged worker fails
// the test instead of hanging the suite.
static void wait_for_parse(Analyzer& analyzer, const std::string& uri) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        auto state = analyzer.get_state(uri);
        if (state && state->tree) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    FAIL("timed out waiting for the parse of " << uri << " to commit");
}

TEST_CASE("foldingRange: an edit in flight folds the same as once it settles",
          "[folding]") {
    const std::string uri  = "file:///fold_reparse.sv";
    const std::string text = folding_scaling_source(40);

    Analyzer analyzer;
    analyzer.open(uri, text);
    REQUIRE(!provide_folding_range(analyzer, make_params(uri)).empty());

    // enqueue_parse() installs a text-only snapshot and hands the parse to a
    // worker.  That window is what an editor's didChange-triggered request
    // lands in; pausing the worker holds it open for the length of the check.
    analyzer.set_parse_paused(true);
    analyzer.enqueue_parse(uri, text + "\n// edit\n");

    auto state = analyzer.get_state(uri);
    REQUIRE(state != nullptr);
    REQUIRE(state->tree == nullptr);

    const auto in_flight = provide_folding_range(analyzer, make_params(uri));
    CHECK(!in_flight.empty());

    analyzer.set_parse_paused(false);
    wait_for_parse(analyzer, uri);

    CHECK(same_folds(provide_folding_range(analyzer, make_params(uri)), in_flight));
}

TEST_CASE("foldingRange: a buffer whose first parse is in flight still folds",
          "[folding]") {
    const std::string uri = "file:///fold_first_parse.sv";

    Analyzer analyzer;

    // Nothing open: there is no document to answer for.
    CHECK(provide_folding_range(analyzer, make_params(uri)).empty());

    analyzer.set_parse_paused(true);
    analyzer.enqueue_parse(uri, folding_scaling_source(40));

    auto state = analyzer.get_state(uri);
    REQUIRE(state != nullptr);
    REQUIRE(state->tree == nullptr);

    // The very first request for a buffer, with no syntax tree yet.
    const auto early = provide_folding_range(analyzer, make_params(uri));
    CHECK(!early.empty());

    analyzer.set_parse_paused(false);
    wait_for_parse(analyzer, uri);

    CHECK(same_folds(provide_folding_range(analyzer, make_params(uri)), early));
}

TEST_CASE("foldingRange: the parse commit keeps the folds the reparse window computed",
          "[folding]") {
    // One keystroke installs two snapshots of the same text -- the text-only
    // placeholder and the parsed state that replaces it -- and the editor
    // requests folds on both sides of that commit.  Folds depend on the text
    // and on nothing else, so the second request must not recompute them.
    const std::string uri  = "file:///fold_commit_carry.sv";
    const std::string text = folding_scaling_source(40);

    Analyzer analyzer;
    analyzer.open(uri, text);

    analyzer.set_parse_paused(true);
    analyzer.enqueue_parse(uri, text + "\n// edit\n");

    auto placeholder = analyzer.get_state(uri);
    REQUIRE(placeholder != nullptr);
    REQUIRE(placeholder->tree == nullptr);             // really in the window
    REQUIRE(placeholder->folding_ranges() == nullptr); // nothing computed yet

    const auto in_flight = provide_folding_range(analyzer, make_params(uri));
    REQUIRE(!in_flight.empty());
    const auto computed = placeholder->folding_ranges();
    REQUIRE(computed != nullptr);

    analyzer.set_parse_paused(false);
    wait_for_parse(analyzer, uri);

    auto parsed = analyzer.get_state(uri);
    REQUIRE(parsed != nullptr);
    REQUIRE(parsed != placeholder); // a new snapshot, as didChange always makes
    REQUIRE(parsed->tree != nullptr);

    // The same vector, not merely an equal one.  Comparing contents would pass
    // just as happily against a snapshot that recomputed them, which is the
    // thing being guarded against.
    CHECK(parsed->folding_ranges() == computed);
    CHECK(same_folds(provide_folding_range(analyzer, make_params(uri)), in_flight));
}

TEST_CASE("foldingRange: a fold slot is shared with the parse, not copied at the commit",
          "[folding]") {
    // The editor asks from the notification itself, so the request thread is
    // normally still computing on the placeholder when the parse commits.  A
    // result copied across at commit time is therefore usually copied while the
    // slot is still empty, and both snapshots compute the same answer anyway --
    // measured on a 57 890-line buffer, two edits in three.
    //
    // Provoke that order deliberately: let the parse commit first, then compute,
    // and require the placeholder to see the result.  Only a shared slot can do
    // that; a copy taken at the commit cannot reach backwards.
    const std::string uri  = "file:///fold_commit_share.sv";
    const std::string text = folding_scaling_source(40);

    Analyzer analyzer;
    analyzer.open(uri, text);

    analyzer.set_parse_paused(true);
    analyzer.enqueue_parse(uri, text + "\n// edit\n");

    auto placeholder = analyzer.get_state(uri);
    REQUIRE(placeholder != nullptr);
    REQUIRE(placeholder->tree == nullptr);
    REQUIRE(placeholder->folding_ranges() == nullptr);

    analyzer.set_parse_paused(false);
    wait_for_parse(analyzer, uri);

    auto parsed = analyzer.get_state(uri);
    REQUIRE(parsed != placeholder);
    REQUIRE(parsed->tree != nullptr);
    REQUIRE(parsed->folding_ranges() == nullptr); // nobody has asked yet

    REQUIRE(!provide_folding_range(analyzer, make_params(uri)).empty());

    const auto computed = parsed->folding_ranges();
    REQUIRE(computed != nullptr);
    CHECK(placeholder->folding_ranges() == computed);
}

// ── the spans the token scan must keep frozen ─────────────────────────────
//
// Folding lexes the buffer itself (lex_fold_tokens) instead of borrowing the
// formatter's collector.  Three spans have to stay frozen for the folds above
// to come out the same, and each of them is a span whose *contents* would open
// folds of their own if the scan let their tokens through.

TEST_CASE("foldingRange: a directive's operands do not fold", "[folding]") {
    const std::string uri = "file:///fold_directive_line.sv";

    Analyzer analyzer;
    analyzer.open(uri, R"(module top;
`ifdef SYNTHESIS
  wire a;
  wire b;
`endif
endmodule
)");

    const auto folds = provide_folding_range(analyzer, make_params(uri));

    // The `ifdef/`endif pair folds, ...
    CHECK(has_fold(folds, 1, 4));
    // ... and nothing starts on a directive line: slang lexes `ifdef and
    // SYNTHESIS as two tokens, and an operand read as an ordinary identifier
    // would open a declaration run of its own.
    for (const auto& f : folds)
        CHECK(f.startLine != 4);
}

TEST_CASE("foldingRange: a multiline define body is one token", "[folding]") {
    const std::string uri = "file:///fold_multiline_define.sv";

    Analyzer analyzer;
    analyzer.open(uri, R"(`define WRAP(x) \
  begin \
    if (x) begin \
      $display("x"); \
    end \
  end
module top;
  initial begin
    $display("hi");
  end
endmodule
)");

    const auto folds = provide_folding_range(analyzer, make_params(uri));

    // The begin/end keywords inside the define body are part of the frozen
    // body, so they open nothing; the real begin/end below still folds.
    for (const auto& f : folds)
        CHECK(f.startLine > 5);
    CHECK(has_fold(folds, 7, 9));
}

TEST_CASE("foldingRange: a format-off region does not fold", "[folding]") {
    const std::string uri = "file:///fold_format_off.sv";

    Analyzer analyzer;
    analyzer.open(uri, R"(module top;
  // verilog-format: off
  initial begin
    $display("frozen");
  end
  // verilog-format: on
  initial begin
    $display("live");
  end
endmodule
)");

    const auto folds = provide_folding_range(analyzer, make_params(uri));

    // Neither the disabled region's begin/end nor its marker comments fold; the
    // block after the region does.
    CHECK(!has_fold(folds, 2, 4));
    for (const auto& f : folds)
        CHECK(f.startLine != 1);
    CHECK(has_fold(folds, 6, 8));
}

TEST_CASE("foldingRange: a macro usage is not a directive line", "[folding]") {
    const std::string uri = "file:///fold_macro_usage.sv";

    Analyzer analyzer;
    analyzer.open(uri, R"(module top;
  `MY_MACRO function int f(input int a);
    return a;
  endfunction
endmodule
)");

    const auto folds = provide_folding_range(analyzer, make_params(uri));

    // A directive freezes the rest of its line so its operands cannot open
    // folds.  A user macro invocation must not: `MY_MACRO is an ordinary token,
    // and the function it precedes still folds.
    CHECK(has_fold(folds, 1, 3));
}

// ── one computation per snapshot ──────────────────────────────────────────

TEST_CASE("foldingRange: a second request for an unchanged buffer reuses the folds",
          "[folding]") {
    const std::string uri = "file:///fold_snapshot_cache.sv";

    Analyzer analyzer;
    analyzer.open(uri, folding_scaling_source(20));

    const auto state = analyzer.get_state(uri);
    REQUIRE(state != nullptr);
    // Nothing has asked for folds yet.
    CHECK(state->folding_ranges() == nullptr);

    const auto first = provide_folding_range(analyzer, make_params(uri));
    REQUIRE(!first.empty());

    const auto cached = state->folding_ranges();
    REQUIRE(cached != nullptr);
    CHECK(same_folds(*cached, first));

    // The second request is answered from that same computation, not a new one.
    const auto second = provide_folding_range(analyzer, make_params(uri));
    CHECK(same_folds(second, first));
    CHECK(state->folding_ranges() == cached);
}

TEST_CASE("foldingRange: an edit gets its own folds, not the previous snapshot's",
          "[folding]") {
    const std::string uri = "file:///fold_snapshot_invalidate.sv";

    Analyzer analyzer;
    analyzer.open(uri, folding_scaling_source(20));
    const auto before = provide_folding_range(analyzer, make_params(uri));
    REQUIRE(!before.empty());

    analyzer.change(uri, folding_scaling_source(40));
    const auto state = analyzer.get_state(uri);
    REQUIRE(state != nullptr);
    // A new snapshot starts with nothing cached; the text is what it folds.
    CHECK(state->folding_ranges() == nullptr);

    const auto after = provide_folding_range(analyzer, make_params(uri));
    CHECK(after.size() > before.size());
    REQUIRE(state->folding_ranges() != nullptr);
    CHECK(same_folds(*state->folding_ranges(), after));
}

// ── folds stay inside the buffer ──────────────────────────────────────────

// Index of the last line the editor actually holds.  A buffer whose text ends
// in '\n' has no line after it: "a\nb\n" is two lines, not three.
static int last_line_of(std::string_view text) {
    if (text.empty()) return 0;
    const int newlines = (int)std::count(text.begin(), text.end(), '\n');
    return text.back() == '\n' ? std::max(0, newlines - 1) : newlines;
}

static void check_folds_in_range(const std::vector<FoldingRange>& folds, std::string_view text) {
    const int last = last_line_of(text);
    for (const auto& r : folds) {
        INFO("fold " << r.startLine << "-" << r.endLine << " kind=" << r.kind
                     << " last line=" << last);
        CHECK(r.startLine <= last);
        CHECK(r.endLine <= last);
    }
}

TEST_CASE("foldingRange: an unterminated block comment does not fold past the last line",
          "[folding]") {
    Analyzer          analyzer;
    std::string       uri  = "file:///tmp/fold_unterminated_block.sv";
    const std::string text = R"(module top;
endmodule
/* this comment
   is never closed
)";
    analyzer.open(uri, text);
    auto folds = provide_folding_range(analyzer, make_params(uri));
    check_folds_in_range(folds, text);
    // It still folds -- just to the real last line (3), not one past it.
    CHECK(has_fold_kind(folds, 2, 3, "comment"));
}

TEST_CASE("foldingRange: an unterminated string does not fold past the last line", "[folding]") {
    Analyzer          analyzer;
    std::string       uri  = "file:///tmp/fold_unterminated_string.sv";
    const std::string text = R"(module top;
  initial $display("oops
endmodule
)";
    analyzer.open(uri, text);
    check_folds_in_range(provide_folding_range(analyzer, make_params(uri)), text);
}

TEST_CASE("foldingRange: a truncated buffer does not fold past the last line", "[folding]") {
    const std::vector<std::string> sources = {
        "module top;\n  logic a;\n",
        "module top;\n  case (x)\n",
        "module top;\n  initial begin\n",
        "`ifdef FOO\nmodule top;\n",
        "module top;\n  function int f(\n",
        "// trailing comment run\n// second line\n",
        "/* closed */\n",
        "",
        "module top;\nendmodule",
    };
    for (size_t i = 0; i < sources.size(); ++i) {
        Analyzer    analyzer;
        std::string uri = "file:///tmp/fold_truncated_" + std::to_string(i) + ".sv";
        analyzer.open(uri, sources[i]);
        INFO("source #" << i);
        check_folds_in_range(provide_folding_range(analyzer, make_params(uri)), sources[i]);
    }
}
