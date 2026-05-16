#include "analyzer.hpp"
#include "features/autowire.hpp"
#include "syntax_index.hpp"
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
// AutoFF tests — stub
TEST_CASE("autoff stub", "[autoff]") { REQUIRE(1 == 1); }

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
    const std::string uri = "file:///tmp/lazyverilog_autowire_top.sv";
    analyzer.open(uri, "module top;\n"
                       "    child u_child(.dout(child_dout));\n"
                       "endmodule\n");

    auto state = analyzer.get_state(uri);
    REQUIRE(state);
    REQUIRE(state->tree);
    auto index = state->index;
    analyzer.merge_extra_file_modules(index);

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

    auto updated = autowire_apply(*state, state->index, AutowireOptions{});
    CHECK(updated == state->text);

    updated = autowire_apply(*state, state->index, AutowireOptions{}, 4);
    CHECK(updated.find("module inv(input logic i_a);\nlogic i_d;\n") != std::string::npos);
}
