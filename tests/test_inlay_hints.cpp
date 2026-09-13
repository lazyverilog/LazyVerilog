#include "analyzer.hpp"
#include "features/inlay_hints.hpp"
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>

static const std::string kInlaySource = R"(module child(
    input logic [7:0] a,
    output logic [3:0] b,
    inout wire c
);
endmodule

module top;
    child u_child (
        .a(sig_a),
        .b(sig_b),
        .c(sig_c)
    );
endmodule
)";

TEST_CASE("inlay hints: instance coverage and direction-only port metadata", "[inlay]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/inlay_top.sv";
    analyzer.open(uri, kInlaySource);

    auto hints = provide_inlay_hints(analyzer, uri, 0, 20);

    REQUIRE(hints.size() == 4);

    CHECK(hints[0].position.line == 8);
    CHECK(hints[0].label == "3/3 ports");
    REQUIRE(hints[0].kind.has_value());
    CHECK(*hints[0].kind == lsInlayHintKind::Parameter);
    REQUIRE(hints[0].paddingLeft.has_value());
    CHECK(*hints[0].paddingLeft == true);
    REQUIRE(hints[0].paddingRight.has_value());
    CHECK(*hints[0].paddingRight == false);

    CHECK(hints[1].position.line == 9);
    CHECK(hints[1].position.character == 9);
    CHECK(hints[1].label == "◀");
    REQUIRE(hints[1].kind.has_value());
    CHECK(*hints[1].kind == lsInlayHintKind::Type);
    REQUIRE(hints[1].paddingLeft.has_value());
    CHECK(*hints[1].paddingLeft == false);
    REQUIRE(hints[1].paddingRight.has_value());
    CHECK(*hints[1].paddingRight == false);

    CHECK(hints[2].position.line == 10);
    CHECK(hints[2].position.character == 9);
    CHECK(hints[2].label == "▶");

    CHECK(hints[3].position.line == 11);
    CHECK(hints[3].position.character == 9);
    CHECK(hints[3].label == "↔");
}

TEST_CASE("inlay hints: header parameters do not count toward the port total", "[inlay]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/inlay_param_top.sv";
    analyzer.open(uri, R"(module child #(
    parameter WIDTH = 8
)(
    input logic [WIDTH-1:0] a,
    input logic b,
    output logic c,
    inout wire d
);
endmodule

module top;
    child u_child (
        .a(sig_a),
        .b(sig_b),
        .c(sig_c),
        .d(sig_d)
    );
endmodule
)");

    auto hints = provide_inlay_hints(analyzer, uri, 0, 20);

    REQUIRE(hints.size() == 5);
    CHECK(hints[0].label == "4/4 ports");
}

TEST_CASE("inlay hints: unknown port direction is shown as question mark", "[inlay]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/inlay_unknown_top.sv";
    analyzer.open(uri, R"(module child(logic undirected);
endmodule

module top;
    child u_child (
        .undirected(sig)
    );
endmodule
)");

    auto hints = provide_inlay_hints(analyzer, uri, 0, 20);

    REQUIRE(hints.size() == 2);
    CHECK(hints[0].label == "1/1 ports");
    CHECK(hints[1].position.line == 5);
    CHECK(hints[1].position.character == 9);
    CHECK(hints[1].label == "?");
}

TEST_CASE("inlay hints: stale extra instance connections are shown as question mark",
          "[inlay]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/inlay_stale_top.sv";
    analyzer.open(uri, R"(module child(input logic known);
endmodule

module top;
    child u_child (
        .known(sig_known),
        .stale(sig_stale)
    );
endmodule
)");

    auto hints = provide_inlay_hints(analyzer, uri, 0, 20);

    REQUIRE(hints.size() == 3);
    CHECK(hints[0].label == "1/1 ports");
    CHECK(hints[1].position.line == 5);
    CHECK(hints[1].position.character == 9);
    CHECK(hints[1].label == "◀");
    CHECK(hints[2].position.line == 6);
    CHECK(hints[2].position.character == 9);
    CHECK(hints[2].label == "?");
}

TEST_CASE("inlay hints: respects requested visible range", "[inlay]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/inlay_top.sv";
    analyzer.open(uri, kInlaySource);

    auto hints = provide_inlay_hints(analyzer, uri, 10, 10);

    REQUIRE(hints.size() == 1);
    CHECK(hints[0].position.line == 10);
    CHECK(hints[0].label == "▶");
}

TEST_CASE("inlay hints: resolves module ports from extra files", "[inlay]") {
    const auto extra_path = std::filesystem::temp_directory_path() / "lazyverilog_inlay_child.sv";
    {
        std::ofstream out(extra_path);
        out << R"(module extra_child(
    input logic req,
    output logic ack
);
endmodule
)";
    }

    const std::string top = R"(module top;
    extra_child u_extra (
        .req(req),
        .ack(ack)
    );
endmodule
)";

    Analyzer analyzer;
    analyzer.set_extra_files({extra_path.string()});
    analyzer.wait_for_background_index_idle();
    const std::string uri = "file:///tmp/inlay_extra_top.sv";
    analyzer.open(uri, top);

    auto hints = provide_inlay_hints(analyzer, uri, 0, 10);

    REQUIRE(hints.size() == 3);
    CHECK(hints[0].label == "2/2 ports");
    CHECK(hints[1].label == "◀");
    CHECK(hints[2].label == "▶");

    std::filesystem::remove(extra_path);
}

TEST_CASE("inlay hints: resolves non-ANSI module ports from extra files", "[inlay]") {
    const auto extra_path = std::filesystem::temp_directory_path() / "lazyverilog_inlay_memory.sv";
    {
        std::ofstream out(extra_path);
        out << R"(module memory(
    i_clk, address, data_in, data_out, read_write, chip_en
);
input i_clk;
input wire [5:0] address;
input [7:0] data_in;
output logic [7:0] data_out;
input wire read_write, chip_en;
endmodule
)";
    }

    const std::string top = R"(module memory_top;
memory u_mem (
    .i_clk      (clk),
    .address    (addr),
    .data_in    (din),
    .data_out   (dout),
    .read_write (we),
    .chip_en    (ce)
);
endmodule
)";

    Analyzer analyzer;
    analyzer.set_extra_files({extra_path.string()});
    analyzer.wait_for_background_index_idle();
    const std::string uri = "file:///tmp/inlay_memory_top.sv";
    analyzer.open(uri, top);

    auto hints = provide_inlay_hints(analyzer, uri, 0, 20);

    REQUIRE(hints.size() == 7);
    CHECK(hints[0].label == "6/6 ports");
    CHECK(hints[1].label == "◀");
    CHECK(hints[2].label == "◀");
    CHECK(hints[4].label == "▶");

    std::filesystem::remove(extra_path);
}

TEST_CASE("inlay hints: covers instances inside generate blocks", "[inlay]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/inlay_generate_top.sv";
    analyzer.open(uri, R"(module child(
    input logic req,
    output logic ack
);
endmodule

module top;
    for (genvar i = 0; i < 2; i++) begin : gen_loop
        child u_child (
            .req(sig_req),
            .ack(sig_ack)
        );
    end
endmodule
)");

    auto hints = provide_inlay_hints(analyzer, uri, 0, 20);

    REQUIRE(hints.size() == 3);
    CHECK(hints[0].position.line == 8);
    CHECK(hints[0].label == "2/2 ports");
    CHECK(hints[1].position.line == 9);
    CHECK(hints[1].label == "◀");
    CHECK(hints[2].position.line == 10);
    CHECK(hints[2].label == "▶");
}

TEST_CASE("inlay hints: each label is placed at its own connection expression", "[inlay]") {
    const auto extra_path = std::filesystem::temp_directory_path() / "lazyverilog_inlay_wide.sv";
    {
        std::ofstream out(extra_path);
        out << R"(module memory(address, read_write);
input wire [5:0] address;
input read_write;
endmodule
)";
    }

    const std::string top = R"(module memory_top;
memory u_memory (
    .address(addr),
    .read_write(read_wsssrite)
);
endmodule
)";

    Analyzer analyzer;
    analyzer.set_extra_files({extra_path.string()});
    analyzer.wait_for_background_index_idle();
    const std::string uri = "file:///tmp/inlay_wide_top.sv";
    analyzer.open(uri, top);

    auto hints = provide_inlay_hints(analyzer, uri, 0, 10);

    REQUIRE(hints.size() == 3);
    CHECK(hints[1].position.line == 2);
    CHECK(hints[1].position.character == 5);
    CHECK(hints[1].label == "◀");
    CHECK(hints[2].position.line == 3);
    CHECK(hints[2].position.character == 5);
    CHECK(hints[2].label == "◀");

    std::filesystem::remove(extra_path);
}

// ── Hints while a parse is in flight ────────────────────────────────────────
//
// An editor asks for hints from the `didChange` notification itself, so the
// request reliably arrives while the parse that notification started is still
// running and the snapshot carries text but no syntax tree.
//
// Hints are derived from the AST and cannot be produced without one, so --
// unlike folds, which leave the AST entirely -- the answer here is to wait for
// the parse, and failing that to serve the snapshot the user was looking at a
// keystroke ago.  Returning nothing is what this replaces, and it was not a
// harmless failure: the client renders the empty reply, so hints blinked out
// for as long as the user kept typing and came back only when an unrelated
// project-index publish happened to fire a refresh.
//
// Both tests hold the parse worker with set_parse_paused() so the window is a
// state they enter rather than one they race the worker for -- retrying until
// the worker happened to lose is not a race a test can win on a one-CPU slice,
// where the scheduler that runs the worker first runs it first on every
// attempt.

#include <chrono>
#include <thread>

TEST_CASE("inlay hints: an edit in flight still produces hints", "[inlay]") {
    const std::string uri = "file:///tmp/inlay_reparse.sv";

    Analyzer analyzer;
    analyzer.open(uri, kInlaySource);
    const auto settled = provide_inlay_hints(analyzer, uri, 0, 20);
    REQUIRE(settled.size() == 4);

    // Appending comments below the instance leaves every hint where it was, so
    // the in-flight answer and the settled one are comparable directly.
    analyzer.set_parse_paused(true);
    analyzer.enqueue_parse(uri, kInlaySource + "\n// edit\n");

    auto state = analyzer.get_state(uri);
    REQUIRE(state != nullptr);
    REQUIRE(state->tree == nullptr);

    const auto in_flight = provide_inlay_hints(analyzer, uri, 0, 20);
    REQUIRE(in_flight.size() == settled.size());
    for (size_t i = 0; i < in_flight.size(); ++i) {
        CHECK(in_flight[i].label == settled[i].label);
        CHECK(in_flight[i].position.line == settled[i].position.line);
        CHECK(in_flight[i].position.character == settled[i].position.character);
    }

    analyzer.set_parse_paused(false);
}

TEST_CASE("inlay hints: a text-only snapshot keeps the parse it replaced", "[inlay]") {
    // The fallback the bounded wait lands on when the user types faster than
    // the file parses.  Asked with no time to wait at all, the answer still has
    // to come from a tree -- the previous one.
    const std::string uri = "file:///tmp/inlay_fallback.sv";

    Analyzer analyzer;
    analyzer.open(uri, kInlaySource);
    REQUIRE(provide_inlay_hints(analyzer, uri, 0, 20).size() == 4);

    analyzer.set_parse_paused(true);
    analyzer.enqueue_parse(uri, kInlaySource + "\n// edit\n");

    auto placeholder = analyzer.get_state(uri);
    REQUIRE(placeholder != nullptr);
    REQUIRE(placeholder->tree == nullptr);

    // Carried forward by enqueue_parse(), and never deeper than one.
    REQUIRE(placeholder->previous_parsed != nullptr);
    CHECK(placeholder->previous_parsed->tree != nullptr);
    CHECK(placeholder->previous_parsed->previous_parsed == nullptr);

    const auto immediate = analyzer.get_parsed_state(uri, std::chrono::milliseconds(0));
    REQUIRE(immediate != nullptr);
    CHECK(immediate->tree != nullptr);

    analyzer.set_parse_paused(false);
}

TEST_CASE("inlay hints: a burst of edits keeps the chain exactly one deep", "[inlay]") {
    // enqueue_parse() takes the predecessor's own predecessor when the
    // predecessor is itself a placeholder.  With the worker held, every edit in
    // the burst lands on a placeholder, which is the case that would grow the
    // chain if it took the predecessor unconditionally.
    const std::string uri = "file:///tmp/inlay_burst.sv";

    Analyzer analyzer;
    analyzer.open(uri, kInlaySource);
    REQUIRE(provide_inlay_hints(analyzer, uri, 0, 20).size() == 4);
    const auto parsed = analyzer.get_state(uri);
    REQUIRE(parsed->tree != nullptr);

    analyzer.set_parse_paused(true);
    for (int i = 0; i < 8; ++i) {
        analyzer.enqueue_parse(uri, kInlaySource + "\n// edit " + std::to_string(i) + "\n");

        auto placeholder = analyzer.get_state(uri);
        REQUIRE(placeholder != nullptr);
        REQUIRE(placeholder->tree == nullptr);
        // Always the one snapshot that parsed, never a chain of placeholders.
        REQUIRE(placeholder->previous_parsed == parsed);
        CHECK(placeholder->previous_parsed->previous_parsed == nullptr);

        // And the hints keep coming from it for the whole burst.
        CHECK(provide_inlay_hints(analyzer, uri, 0, 20).size() == 4);
    }

    analyzer.set_parse_paused(false);
}

TEST_CASE("inlay hints: a document that was never opened answers nothing", "[inlay]") {
    // Guards the fallback against reaching into some other document: with no
    // snapshot at all there is genuinely nothing to say.
    Analyzer analyzer;
    analyzer.open("file:///tmp/inlay_other.sv", kInlaySource);
    REQUIRE(provide_inlay_hints(analyzer, "file:///tmp/inlay_other.sv", 0, 20).size() == 4);

    CHECK(analyzer.get_parsed_state("file:///tmp/inlay_absent.sv") == nullptr);
    CHECK(provide_inlay_hints(analyzer, "file:///tmp/inlay_absent.sv", 0, 20).empty());
}
