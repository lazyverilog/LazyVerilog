#include "analyzer.hpp"
#include "features/autofunc.hpp"
#include "features/code_action.hpp"
#include "features/hover.hpp"
#include "features/signature_help.hpp"
#include "features/workspace_symbols.hpp"
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>

TEST_CASE("hover: formats symbol info as markdown", "[hover]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/hover_fixture.sv";
    analyzer.open(uri, R"(
module top(input logic [3:0] a);
endmodule
)");

    lsTextDocumentPositionParams params;
    params.textDocument.uri.raw_uri_ = uri;
    params.position = lsPosition(1, 29);

    auto hover = provide_hover(analyzer, params);
    REQUIRE(hover.has_value());
    REQUIRE(hover->contents.second.has_value());
    CHECK(hover->contents.second->value.find("**a**") != std::string::npos);
    CHECK(hover->contents.second->value.find("input logic [3:0]") != std::string::npos);
}

TEST_CASE("hover: resolves instance module names through extra files", "[hover]") {
    const auto path = std::filesystem::temp_directory_path() / "lazyverilog_hover_child.sv";
    {
        std::ofstream out(path);
        out << "module child(input logic clk);\nendmodule\n";
    }

    Analyzer analyzer;
    const std::string uri = "file:///tmp/hover_top.sv";
    analyzer.set_extra_files({path.string()});
    analyzer.open(uri, R"(
module top;
    child u_child();
endmodule
)");

    lsTextDocumentPositionParams params;
    params.textDocument.uri.raw_uri_ = uri;
    params.position = lsPosition(2, 6);

    auto hover = provide_hover(analyzer, params);
    REQUIRE(hover.has_value());
    REQUIRE(hover->contents.second.has_value());
    CHECK(hover->contents.second->value.find("**child**") != std::string::npos);
    CHECK(hover->contents.second->value.find("*module*") != std::string::npos);

    std::filesystem::remove(path);
}

TEST_CASE("rtltree: builds forward hierarchy from open documents", "[rtltree]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/rtltree_top.sv";
    analyzer.open(uri, R"(
module top;
    mid u_mid();
endmodule

module leaf;
endmodule

module mid;
    leaf u_leaf();
endmodule
)");

    auto tree = analyzer.rtl_tree(uri);
    REQUIRE(tree.has_value());
    CHECK(tree->name == "top");
    CHECK(tree->file == uri);
    REQUIRE(tree->children.size() == 1);
    CHECK(tree->children[0].name == "mid");
    CHECK(tree->children[0].inst == "u_mid");
    REQUIRE(tree->children[0].children.size() == 1);
    CHECK(tree->children[0].children[0].name == "leaf");
    CHECK(tree->children[0].children[0].inst == "u_leaf");
}

TEST_CASE("rtltree: builds reverse hierarchy through extra files", "[rtltree]") {
    const auto top_path = std::filesystem::temp_directory_path() / "lazyverilog_rtltree_top.sv";
    {
        std::ofstream out(top_path);
        out << "module top;\n"
               "    leaf u_leaf();\n"
               "endmodule\n";
    }

    Analyzer analyzer;
    analyzer.set_extra_files({top_path.string()});
    const std::string leaf_uri = "file:///tmp/lazyverilog_rtltree_leaf.sv";
    analyzer.open(leaf_uri, "module leaf;\nendmodule\n");

    auto tree = analyzer.rtl_tree_reverse(leaf_uri);
    REQUIRE(tree.has_value());
    CHECK(tree->name == "leaf");
    REQUIRE(tree->children.size() == 1);
    CHECK(tree->children[0].name == "top");
    CHECK(tree->children[0].inst == "u_leaf");

    std::filesystem::remove(top_path);
}

TEST_CASE("hover: resolves variable and function/task call symbols", "[hover]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/hover_calls.sv";
    analyzer.open(uri, R"(
function int add(input int a, input int b);
    return a + b;
endfunction

task run(input int a);
endtask

module top;
    logic [2:0] value;
    initial begin
        value = add(value, 1);
        run(value);
    end
endmodule
)");

    auto hover_at = [&](int line, int col) {
        lsTextDocumentPositionParams params;
        params.textDocument.uri.raw_uri_ = uri;
        params.position = lsPosition(line, col);
        return provide_hover(analyzer, params);
    };

    auto variable_hover = hover_at(11, 10);
    REQUIRE(variable_hover.has_value());
    REQUIRE(variable_hover->contents.second.has_value());
    CHECK(variable_hover->contents.second->value == "**value** — *variable*");

    auto function_hover = hover_at(11, 18);
    REQUIRE(function_hover.has_value());
    REQUIRE(function_hover->contents.second.has_value());
    CHECK(function_hover->contents.second->value ==
          "**add** — *function*\n\n---\n\n```\nfunction int add(\n    input int a,\n    input int "
          "b\n)\n```");

    auto task_hover = hover_at(12, 10);
    REQUIRE(task_hover.has_value());
    REQUIRE(task_hover->contents.second.has_value());
    CHECK(task_hover->contents.second->value ==
          "**run** — *task*\n\n---\n\n```\ntask run(input int a)\n```");
}

TEST_CASE("hover: shows parameter values and macro bodies", "[hover]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/hover_parameter_macro.sv";
    analyzer.open(uri, R"(
`define WIDTH 32
module top;
    parameter int DEPTH = 8;
    logic [`WIDTH-1:0] data;
    logic [DEPTH-1:0] addr;
endmodule
)");

    auto hover_at = [&](int line, int col) {
        lsTextDocumentPositionParams params;
        params.textDocument.uri.raw_uri_ = uri;
        params.position = lsPosition(line, col);
        return provide_hover(analyzer, params);
    };

    auto macro_hover = hover_at(4, 13);
    REQUIRE(macro_hover.has_value());
    REQUIRE(macro_hover->contents.second.has_value());
    CHECK(macro_hover->contents.second->value == "**WIDTH** — *macro*\n\n---\n\n```\n32\n```");

    auto parameter_hover = hover_at(5, 12);
    REQUIRE(parameter_hover.has_value());
    REQUIRE(parameter_hover->contents.second.has_value());
    CHECK(parameter_hover->contents.second->value ==
          "**DEPTH** — *parameter*\n\n---\n\n```\nint = 8\n```");
}

TEST_CASE("signature help: returns function argument labels and active parameter", "[signature]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/signature_fixture.sv";
    analyzer.open(uri, R"(
function int add(input int a, input int b);
    return a + b;
endfunction

module top;
    initial add(foo, );
endmodule
)");

    lsTextDocumentPositionParams params;
    params.textDocument.uri.raw_uri_ = uri;
    params.position = lsPosition(6, 21);

    auto help = provide_signature_help(analyzer, params);
    REQUIRE(help.has_value());
    REQUIRE(help->signatures.size() == 1);
    CHECK(help->signatures[0].label == "function int add(input int a, input int b)");
    REQUIRE(help->activeParameter.has_value());
    CHECK(*help->activeParameter == 1);
}

TEST_CASE("workspace symbols: indexes top-level symbols from extra files", "[workspace]") {
    const auto path = std::filesystem::temp_directory_path() / "lazyverilog_workspace_symbols.sv";
    {
        std::ofstream out(path);
        out << R"(
module alpha;
endmodule

interface axi_if;
endinterface

package pkt_pkg;
endpackage

class packet_class;
endclass
)";
    }

    Analyzer analyzer;
    analyzer.set_extra_files({path.string()});

    WorkspaceSymbolParams params;
    params.query = "p";
    auto symbols = provide_workspace_symbols(analyzer, params);

    REQUIRE(symbols.size() == 3);
    CHECK(symbols[0].name == "alpha");
    CHECK(symbols[0].kind == lsSymbolKind::Module);
    CHECK(symbols[1].name == "pkt_pkg");
    CHECK(symbols[1].kind == lsSymbolKind::Package);
    CHECK(symbols[2].name == "packet_class");
    CHECK(symbols[2].kind == lsSymbolKind::Class);

    std::filesystem::remove(path);
}

TEST_CASE("autofunc: preserves positional call arguments", "[autofunc]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/autofunc_positional.sv";
    analyzer.open(uri, R"(
function int sum(input int i_a, input int i_b);
    return i_a + i_b;
endfunction

module top;
    initial begin
        sum(1, 2);
    end
endmodule
)");

    AutoFuncOptions options;
    options.indent_size = 4;
    options.use_named_arguments = true;

    auto edit = autofunc(analyzer, uri, 7, 9, options);
    REQUIRE(edit.has_value());
    REQUIRE(edit->changes.has_value());
    auto it = edit->changes->find(uri);
    REQUIRE(it != edit->changes->end());
    REQUIRE(it->second.size() == 1);
    CHECK(it->second[0].newText == "sum(\n            .i_a(1),\n            .i_b(2)\n        );");
}

TEST_CASE("autofunc code action formats replacement at insertion column", "[autofunc]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/autofunc_code_action_indent.sv";
    analyzer.open(uri, R"(
task add_number(input int a, input int b, output int result);
    result = a + b;
endtask

module top;
    initial begin
        add_number(.a(a3),
                   .b(b),
                   .result(result));
    end
endmodule
)");

    Config config;
    config.format.indent_size = 4;
    config.format.function.arg_count = 3;
    config.format.function.layout = "hanging";
    config.autofunc.indent_size = 4;
    config.autofunc.use_named_arguments = true;

    lsCodeActionParams params;
    params.textDocument.uri.raw_uri_ = uri;
    params.range.start = lsPosition(7, 9);

    auto actions = provide_code_actions(analyzer, config, params);
    auto it = std::find_if(actions.begin(), actions.end(), [](const CodeAction& action) {
        return action.title == "AutoFunc: expand function call";
    });
    REQUIRE(it != actions.end());
    REQUIRE(it->edit.has_value());
    REQUIRE(it->edit->changes.has_value());
    auto change = it->edit->changes->find(uri);
    REQUIRE(change != it->edit->changes->end());
    REQUIRE(change->second.size() == 1);
    CHECK(change->second[0].newText ==
          "add_number(.a(a3),\n"
          "                   .b(b),\n"
          "                   .result(result));");
}
