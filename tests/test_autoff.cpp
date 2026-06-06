#include "analyzer.hpp"
#include "dynamic_file_index.hpp"
#include "features/autoff.hpp"
#include "features/autowire.hpp"
#include "syntax_index.hpp"
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
TEST_CASE("autoff accepts unambiguous register-pattern pairs in either declaration order", "[autoff]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/lazyverilog_autoff_pair_order.sv";
    analyzer.open(uri,
                  "module top;\n"
                  "logic b, r_a;\n"
                  "logic r_c, d;\n"
                  "always_ff @(posedge user_clk or negedge user_reset_n) begin\n"
                  "    if (!user_reset_n) begin\n"
                  "    end else begin\n"
                  "    end\n"
                  "end\n"
                  "endmodule\n");

    auto state = analyzer.get_state(uri);
    REQUIRE(state);
    REQUIRE(state->tree);

    // Clock/reset names are intentionally arbitrary here. AutoFF should unwrap
    // the event-control syntax and pair only by register_pattern, not by signal
    // order or hard-coded clock/reset names.
    auto first = preview_autoff(*state, 1, "^r_");
    REQUIRE_FALSE(first.has_error);
    REQUIRE_FALSE(first.warn);
    REQUIRE(first.pairs.size() == 1);
    CHECK(first.pairs[0].src == "b");
    CHECK(first.pairs[0].dst == "r_a");

    auto second = preview_autoff(*state, 2, "^r_");
    REQUIRE_FALSE(second.has_error);
    REQUIRE_FALSE(second.warn);
    REQUIRE(second.pairs.size() == 1);
    CHECK(second.pairs[0].src == "d");
    CHECK(second.pairs[0].dst == "r_c");
}

TEST_CASE("autoff skips ambiguous two-signal declarations", "[autoff]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/lazyverilog_autoff_ambiguous.sv";
    analyzer.open(uri,
                  "module top;\n"
                  "logic a, b;\n"
                  "logic r_a, r_b;\n"
                  "always_ff @(posedge clk or negedge rst_n) begin\n"
                  "    if (!rst_n) begin\n"
                  "    end else begin\n"
                  "    end\n"
                  "end\n"
                  "endmodule\n");

    auto state = analyzer.get_state(uri);
    REQUIRE(state);
    REQUIRE(state->tree);

    auto no_match = autoff(*state, 1, "^r_");
    CHECK_FALSE(no_match.has_error);
    CHECK(no_match.warn);
    CHECK(no_match.edits.empty());
    CHECK(no_match.error.find("exactly one signal matching register_pattern") != std::string::npos);

    auto both_match = preview_autoff(*state, 2, "^r_");
    CHECK_FALSE(both_match.has_error);
    CHECK(both_match.warn);
    CHECK(both_match.pairs.empty());
    CHECK(both_match.error.find("exactly one signal matching register_pattern") != std::string::npos);
}

TEST_CASE("autowire uses cached extra-file modules", "[autowire]") {
    const auto extra_path =
        std::filesystem::temp_directory_path() / "lazyverilog_autowire_child.sv";
    {
        std::ofstream out(extra_path);
        REQUIRE(out.good());
        out << "module child(output logic [7:0] dout);\nendmodule\n";
    }

    Analyzer analyzer;
    analyzer.set_extra_files({extra_path.string()});
    analyzer.wait_for_background_index_idle();
    const std::string uri = "file:///tmp/lazyverilog_autowire_top.sv";
    analyzer.open(uri, "module top;\n"
                       "    child u_child(.dout(child_dout));\n"
                       "endmodule\n");

    auto state = analyzer.get_state(uri);
    REQUIRE(state);
    REQUIRE(state->tree);
    auto index = build_dynamic_file_index(*state);
    if (auto project = analyzer.extra_project_index())
        index.merge(*project);

    auto updated = autowire_apply(*state, index, AutowireOptions{});
    CHECK(updated.find("logic [7:0] child_dout;") != std::string::npos);

    std::filesystem::remove(extra_path);
}

TEST_CASE("autowire ignores missing signals in later modules", "[autowire]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/lazyverilog_autowire_two_modules.sv";
    analyzer.open(uri, "module top;\n"
                       "endmodule\n"
                       "\n"
                       "module inv(input logic i_a);\n"
                       "assign i_d = ~i_a;\n"
                       "endmodule\n");

    auto state = analyzer.get_state(uri);
    REQUIRE(state);
    REQUIRE(state->tree);

    auto updated = autowire_apply(*state, build_dynamic_file_index(*state), AutowireOptions{});
    CHECK(updated == state->text);

    updated = autowire_apply(*state, build_dynamic_file_index(*state), AutowireOptions{}, 4);
    CHECK(updated.find("module inv(input logic i_a);\nlogic i_d;\n") != std::string::npos);
}
