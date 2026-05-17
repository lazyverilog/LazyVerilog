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
        .b(sig_b)
    );
endmodule
)";

TEST_CASE("inlay hints: instance coverage and aligned port metadata", "[inlay]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/inlay_top.sv";
    analyzer.open(uri, kInlaySource);

    auto hints = provide_inlay_hints(analyzer, uri, 0, 20);

    REQUIRE(hints.size() == 3);

    CHECK(hints[0].position.line == 8);
    CHECK(hints[0].label == "2/3 ports");
    REQUIRE(hints[0].kind.has_value());
    CHECK(*hints[0].kind == lsInlayHintKind::Parameter);
    REQUIRE(hints[0].paddingLeft.has_value());
    CHECK(*hints[0].paddingLeft == true);
    REQUIRE(hints[0].paddingRight.has_value());
    CHECK(*hints[0].paddingRight == false);

    CHECK(hints[1].position.line == 9);
    CHECK(hints[1].position.character == 11);
    CHECK(hints[1].label == "input  logic [7:0]");
    REQUIRE(hints[1].kind.has_value());
    CHECK(*hints[1].kind == lsInlayHintKind::Type);
    REQUIRE(hints[1].paddingLeft.has_value());
    CHECK(*hints[1].paddingLeft == false);
    REQUIRE(hints[1].paddingRight.has_value());
    CHECK(*hints[1].paddingRight == true);

    CHECK(hints[2].position.line == 10);
    CHECK(hints[2].position.character == 11);
    CHECK(hints[2].label == "output logic [3:0]");
}

TEST_CASE("inlay hints: respects requested visible range", "[inlay]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/inlay_top.sv";
    analyzer.open(uri, kInlaySource);

    auto hints = provide_inlay_hints(analyzer, uri, 10, 10);

    REQUIRE(hints.size() == 1);
    CHECK(hints[0].position.line == 10);
    CHECK(hints[0].label == "output logic [3:0]");
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
    const std::string uri = "file:///tmp/inlay_extra_top.sv";
    analyzer.open(uri, top);

    auto hints = provide_inlay_hints(analyzer, uri, 0, 10);

    REQUIRE(hints.size() == 3);
    CHECK(hints[0].label == "2/2 ports");
    CHECK(hints[1].label == "input  logic");
    CHECK(hints[2].label == "output logic");

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
    const std::string uri = "file:///tmp/inlay_memory_top.sv";
    analyzer.open(uri, top);

    auto hints = provide_inlay_hints(analyzer, uri, 0, 20);

    REQUIRE(hints.size() == 7);
    CHECK(hints[0].label == "6/6 ports");
    CHECK(hints[1].label == "input             ");
    CHECK(hints[2].label == "input  [5:0]      ");
    CHECK(hints[4].label == "output logic [7:0]");

    std::filesystem::remove(extra_path);
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
    const std::string uri = "file:///tmp/inlay_wide_top.sv";
    analyzer.open(uri, top);

    auto hints = provide_inlay_hints(analyzer, uri, 0, 10);

    REQUIRE(hints.size() == 3);
    CHECK(hints[1].position.line == 2);
    CHECK(hints[1].position.character == 13);
    CHECK(hints[1].label == "input [5:0]");
    CHECK(hints[2].position.line == 3);
    CHECK(hints[2].position.character == 16);
    CHECK(hints[2].label == "input      ");

    std::filesystem::remove(extra_path);
}
