#include "analyzer.hpp"
#include "features/autofunc.hpp"
#include "features/autoinst.hpp"
#include "features/code_action.hpp"
#include "features/hover.hpp"
#include "features/signature_help.hpp"
#include "features/workspace_symbols.hpp"
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <string_view>

static std::pair<int, int> lsp_feature_position_of(std::string_view text,
                                                   std::string_view needle) {
    const auto offset = text.find(needle);
    REQUIRE(offset != std::string_view::npos);
    int line = 0;
    int col = 0;
    for (size_t i = 0; i < offset; ++i) {
        if (text[i] == '\n') {
            ++line;
            col = 0;
        } else {
            ++col;
        }
    }
    return {line, col};
}


TEST_CASE("autoinst: skips module parameters from project index", "[autoinst]") {
    const auto module_path = std::filesystem::temp_directory_path() /
                             "lazyverilog_autoinst_param_memory.sv";
    {
        std::ofstream out(module_path);
        out << R"SV(module memory #(
    parameter WIDTH = 8
)(
    i_clk, address, i_data,
    i_test, o_data, o_test,
    o_test2, o_test3
);
input logic i_clk;
input logic [WIDTH-1:0] address;
input logic [WIDTH-1:0] i_data;
input logic i_test;
output logic [WIDTH-1:0] o_data;
output logic o_test;
output logic o_test2;
output logic o_test3;
endmodule
)SV";
    }

    Analyzer analyzer;
    analyzer.set_extra_files({module_path.string()});
    analyzer.wait_for_background_index_idle();

    const std::string uri = "file:///tmp/lazyverilog_autoinst_param_top.sv";
    const std::string top = R"SV(module top;
    memory u_mem2 ();
endmodule
)SV";
    analyzer.open(uri, top);
    auto state = analyzer.get_state(uri);
    REQUIRE(state);

    auto result = autoinst_impl(*state, 1, 12, nullptr,
                                analyzer.project_index_snapshot().get());
    REQUIRE(result.has_value());
    CHECK(std::find(result->port_names.begin(), result->port_names.end(), "WIDTH") ==
          result->port_names.end());
    CHECK(std::find(result->port_names.begin(), result->port_names.end(), "i_clk") !=
          result->port_names.end());

    const auto formatted = format_autoinst(*result, top, AutoinstOptions{});
    CHECK(formatted.find(".WIDTH") == std::string::npos);
    CHECK(formatted.find(".i_clk") != std::string::npos);
}

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

TEST_CASE("hover: includes unpacked dimensions on ports", "[hover]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/hover_port_dimensions.sv";
    const std::string text = R"(
`define WIDTH 8
`define DEPTH 4
module ansi_top(
    input logic [1:0] ansi_data [7:0],
    output wire [3:0] ansi_net [2],
    input logic [`WIDTH-1:0] ansi_macro_data [`DEPTH]
);
endmodule

module non_ansi_top(
    non_ansi_data,
    non_ansi_b,
    non_ansi_macro
);
input logic [1:0] non_ansi_data [7:0];
input logic [1:0] non_ansi_b [3:0];
input logic [`WIDTH-1:0] non_ansi_macro [`DEPTH];
endmodule
)";
    analyzer.open(uri, text);

    auto position_of = [&](std::string_view needle) {
        const auto offset = text.find(needle);
        REQUIRE(offset != std::string::npos);

        int line = 0;
        int col = 0;
        for (size_t i = 0; i < offset; ++i) {
            if (text[i] == '\n') {
                ++line;
                col = 0;
            } else {
                ++col;
            }
        }
        return lsPosition(line, col);
    };

    auto hover_on = [&](std::string_view needle) {
        lsTextDocumentPositionParams params;
        params.textDocument.uri.raw_uri_ = uri;
        params.position = position_of(needle);
        return provide_hover(analyzer, params);
    };

    auto ansi_data_hover = hover_on("ansi_data");
    REQUIRE(ansi_data_hover.has_value());
    REQUIRE(ansi_data_hover->contents.second.has_value());
    CHECK(ansi_data_hover->contents.second->value ==
          "**ansi_data** — *port*\n\n---\n\n```\ninput logic [1:0] [7:0]\n```");

    auto ansi_net_hover = hover_on("ansi_net");
    REQUIRE(ansi_net_hover.has_value());
    REQUIRE(ansi_net_hover->contents.second.has_value());
    CHECK(ansi_net_hover->contents.second->value ==
          "**ansi_net** — *port*\n\n---\n\n```\noutput [3:0] [2]\n```");

    auto non_ansi_data_hover = hover_on("non_ansi_data [7:0]");
    REQUIRE(non_ansi_data_hover.has_value());
    REQUIRE(non_ansi_data_hover->contents.second.has_value());
    CHECK(non_ansi_data_hover->contents.second->value ==
          "**non_ansi_data** — *port*\n\n---\n\n```\ninput logic [1:0] [7:0]\n```");

    auto non_ansi_b_hover = hover_on("non_ansi_b [3:0]");
    REQUIRE(non_ansi_b_hover.has_value());
    REQUIRE(non_ansi_b_hover->contents.second.has_value());
    CHECK(non_ansi_b_hover->contents.second->value ==
          "**non_ansi_b** — *port*\n\n---\n\n```\ninput logic [1:0] [3:0]\n```");

    auto ansi_macro_hover = hover_on("ansi_macro_data");
    REQUIRE(ansi_macro_hover.has_value());
    REQUIRE(ansi_macro_hover->contents.second.has_value());
    CHECK(ansi_macro_hover->contents.second->value ==
          "**ansi_macro_data** — *port*\n\n---\n\n```\ninput logic [`WIDTH-1:0] [`DEPTH]\n```");

    auto non_ansi_macro_hover = hover_on("non_ansi_macro [`DEPTH]");
    REQUIRE(non_ansi_macro_hover.has_value());
    REQUIRE(non_ansi_macro_hover->contents.second.has_value());
    CHECK(non_ansi_macro_hover->contents.second->value ==
          "**non_ansi_macro** — *port*\n\n---\n\n```\ninput logic [`WIDTH-1:0] [`DEPTH]\n```");
}

TEST_CASE("hover: memory_top style non-ANSI port declaration keeps full dimensions", "[hover]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/hover_memory_top_style_port.sv";
    const std::string text = R"(
module memory_top(
    i_data
);
input     logic      [1:0]       i_data            [7:0]         ; // input
endmodule
)";
    analyzer.open(uri, text);

    const auto offset = text.find("i_data            [7:0]");
    REQUIRE(offset != std::string::npos);

    int line = 0;
    int col = 0;
    for (size_t i = 0; i < offset; ++i) {
        if (text[i] == '\n') {
            ++line;
            col = 0;
        } else {
            ++col;
        }
    }

    lsTextDocumentPositionParams params;
    params.textDocument.uri.raw_uri_ = uri;
    params.position = lsPosition(line, col);

    auto hover = provide_hover(analyzer, params);
    REQUIRE(hover.has_value());
    REQUIRE(hover->contents.second.has_value());

    // Regression for the original demo/memory_top.sv report:
    //
    //     input logic [1:0] i_data [7:0];
    //
    // Hover used to show only the direction and packed range:
    //
    //     input logic [1:0]
    //
    // The unpacked [7:0] dimension is attached to the declarator, not the port
    // header, so the hover renderer and SyntaxIndex must both append it.
    CHECK(hover->contents.second->value ==
          "**i_data** — *port*\n\n---\n\n```\ninput logic [1:0] [7:0]\n```");
}

TEST_CASE("hover: expression lookup stays in current module scope", "[hover]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/hover_scope_duplicate_names.sv";
    const std::string text = R"(
typedef logic [7:0] fifo_entry_t;

module memory_top;
    logic [3:0] o_d;
    logic [3:0] sink;
    assign sink = o_d;
endmodule

module inv(
    i_a, o_d
);
input  fifo_entry_t [3:0] i_a;
output fifo_entry_t [3:0] o_d;
endmodule
)";
    analyzer.open(uri, text);

    const auto offset = text.find("o_d;\nendmodule");
    REQUIRE(offset != std::string::npos);

    int line = 0;
    int col = 0;
    for (size_t i = 0; i < offset; ++i) {
        if (text[i] == '\n') {
            ++line;
            col = 0;
        } else {
            ++col;
        }
    }

    lsTextDocumentPositionParams params;
    params.textDocument.uri.raw_uri_ = uri;
    params.position = lsPosition(line, col);

    auto hover = provide_hover(analyzer, params);
    REQUIRE(hover.has_value());
    REQUIRE(hover->contents.second.has_value());

    // Regression for demo/memory_top.sv:
    //
    //   memory_top declares a local signal named o_d.
    //   inv declares a port with the same name later in the same file.
    //
    // Hover used to take a SyntaxIndex fast path that searched ports by name
    // without checking the current module scope, so hovering memory_top.o_d
    // displayed inv.o_d.  Hover should resolve through definition_of() first so
    // the current module's declaration wins.
    CHECK(hover->contents.second->value ==
          "**o_d** — *variable*\n\n---\n\n```\nlogic [3:0]\n```");
}

TEST_CASE("hover: duplicate port names prefer current module declaration", "[hover]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/hover_scope_duplicate_ports.sv";
    const std::string text = R"(
module first(
    o_d
);
output logic [9:0] o_d;
endmodule

module second(
    o_d
);
output logic [3:0] o_d;
endmodule
)";
    analyzer.open(uri, text);

    const auto offset = text.find("o_d;\nendmodule");
    REQUIRE(offset != std::string::npos);

    int line = 0;
    int col = 0;
    for (size_t i = 0; i < offset; ++i) {
        if (text[i] == '\n') {
            ++line;
            col = 0;
        } else {
            ++col;
        }
    }

    lsTextDocumentPositionParams params;
    params.textDocument.uri.raw_uri_ = uri;
    params.position = lsPosition(line, col);

    auto hover = provide_hover(analyzer, params);
    REQUIRE(hover.has_value());
    REQUIRE(hover->contents.second.has_value());

    // This catches the broader version of the same scope bug: duplicate port
    // names in different modules must not be resolved by the first name-only
    // SyntaxIndex hit.
    CHECK(hover->contents.second->value ==
          "**o_d** — *port*\n\n---\n\n```\noutput logic [9:0]\n```");
}

TEST_CASE("hover: enriches typedefs defined by current-file includes", "[hover]") {
    const auto path = std::filesystem::temp_directory_path() /
                      "lazyverilog_hover_include_params.svh";
    {
        std::ofstream out(path);
        out << R"SV(typedef enum logic [1:0] {
    IDLE,
    RUN
} state_t;
)SV";
    }

    Analyzer analyzer;
    const auto user_path = std::filesystem::temp_directory_path() /
                           "lazyverilog_hover_include_user.sv";
    const std::string uri = "file://" + user_path.string();
    const std::string text = R"SV(`include "lazyverilog_hover_include_params.svh"

module top;
    state_t state;
endmodule
)SV";
    analyzer.open(uri, text);

    lsTextDocumentPositionParams params;
    params.textDocument.uri.raw_uri_ = uri;
    params.position = lsPosition(3, 5);

    auto hover = provide_hover(analyzer, params);
    REQUIRE(hover.has_value());
    REQUIRE(hover->contents.second.has_value());
    CHECK(hover->contents.second->value.find("**state_t**") != std::string::npos);
    CHECK(hover->contents.second->value.find("*typedef*") != std::string::npos);
    CHECK(hover->contents.second->value.find("IDLE") != std::string::npos);

    std::filesystem::remove(path);
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
    analyzer.wait_for_background_index_idle();
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

TEST_CASE("hover: enriches closed-file index symbols beyond bare symbol", "[hover]") {
    const auto path = std::filesystem::temp_directory_path() / "lazyverilog_hover_index_symbols.sv";
    {
        std::ofstream out(path);
        out << R"SV(
package hover_idx_pkg;
    parameter int PKT_W = 32;
    logic [3:0] pkg_var;

    typedef enum logic [1:0] {
        IDLE,
        RUN
    } state_e;

    typedef struct packed {
        logic [7:0] addr;
        logic       valid;
    } packet_s;

    class packet_c;
        int count;
        function int size();
            return count;
        endfunction
    endclass
endpackage

module hover_idx_defs #(
    parameter int WIDTH = 8
)();
endmodule
)SV";
    }

    Analyzer analyzer;
    analyzer.set_extra_files({path.string()});
    analyzer.wait_for_background_index_idle();

    const std::string uri = "file:///tmp/hover_index_user.sv";
    const std::string text = R"SV(
import hover_idx_pkg::*;

module top;
    packet_s pkt;
    packet_c obj;
    state_e st;
    logic [PKT_W-1:0] bus;

    initial begin
        st = RUN;
        bus = pkg_var;
        bus = pkt.addr;
        obj.count = obj.size();
    end

    hover_idx_defs #(.WIDTH(16)) u_defs();
endmodule
)SV";
    analyzer.open(uri, text);

    auto position_of = [&](std::string_view needle) {
        const auto offset = text.find(needle);
        REQUIRE(offset != std::string::npos);

        int line = 0;
        int col = 0;
        for (size_t i = 0; i < offset; ++i) {
            if (text[i] == '\n') {
                ++line;
                col = 0;
            } else {
                ++col;
            }
        }
        return lsPosition(line, col);
    };

    auto hover_value = [&](std::string_view needle) {
        lsTextDocumentPositionParams params;
        params.textDocument.uri.raw_uri_ = uri;
        params.position = position_of(needle);
        auto hover = provide_hover(analyzer, params);
        REQUIRE(hover.has_value());
        REQUIRE(hover->contents.second.has_value());
        return hover->contents.second->value;
    };

    CHECK(hover_value("PKT_W").find("*parameter*") != std::string::npos);
    CHECK(hover_value("PKT_W").find("int") != std::string::npos);
    CHECK(hover_value("packet_s pkt").find("*typedef*") != std::string::npos);
    CHECK(hover_value("packet_s pkt").find("addr") != std::string::npos);
    CHECK(hover_value("RUN").find("*enum_member*") != std::string::npos);
    CHECK(hover_value("pkg_var").find("logic [3:0]") != std::string::npos);
    CHECK(hover_value("addr;").find("logic [7:0]") != std::string::npos);
    CHECK(hover_value("count =").find("int") != std::string::npos);
    CHECK(hover_value("WIDTH(16)").find("*parameter*") != std::string::npos);
    CHECK(hover_value("WIDTH(16)").find("= 8") != std::string::npos);

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
    analyzer.wait_for_background_index_idle();
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
    CHECK(variable_hover->contents.second->value ==
          "**value** — *variable*\n\n---\n\n```\nlogic [2:0]\n```");

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

TEST_CASE("hover: shows data types for module and local variables", "[hover]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/hover_variable_types.sv";
    const std::string text = R"(
`define WIDTH 32
`define WIDTH123 7
module top;
    typedef struct {
        logic [7:0] struct_addr;
        logic       valid;
    } packet_wo_data_t;

    logic /* packed comment must not leak into hover */ [32-1:0] data;
    logic [`WIDTH123:0] macro_addr;
    logic [`WIDTH-1:0] macro_wdata;
    int queue [4];
    logic [8-1:0] wider;

    initial begin
        automatic bit flag;
        data = queue[0];
        flag = wider[0];
    end
endmodule
)";
    analyzer.open(uri, text);

    auto hover_at = [&](int line, int col) {
        lsTextDocumentPositionParams params;
        params.textDocument.uri.raw_uri_ = uri;
        params.position = lsPosition(line, col);
        return provide_hover(analyzer, params);
    };

    auto position_of = [&](std::string_view needle) {
        const auto offset = text.find(needle);
        REQUIRE(offset != std::string::npos);

        int line = 0;
        int col = 0;
        for (size_t i = 0; i < offset; ++i) {
            if (text[i] == '\n') {
                ++line;
                col = 0;
            } else {
                ++col;
            }
        }
        return lsPosition(line, col);
    };

    auto hover_on = [&](std::string_view needle) {
        auto pos = position_of(needle);
        return hover_at(pos.line, pos.character);
    };

    auto struct_addr_hover = hover_on("struct_addr");
    REQUIRE(struct_addr_hover.has_value());
    REQUIRE(struct_addr_hover->contents.second.has_value());
    CHECK(struct_addr_hover->contents.second->value ==
          "**struct_addr** — *variable*\n\n---\n\n```\nlogic [7:0]\n```");

    auto valid_hover = hover_on("valid");
    REQUIRE(valid_hover.has_value());
    REQUIRE(valid_hover->contents.second.has_value());
    CHECK(valid_hover->contents.second->value ==
          "**valid** — *variable*\n\n---\n\n```\nlogic\n```");

    auto data_hover = hover_on("data;");
    REQUIRE(data_hover.has_value());
    REQUIRE(data_hover->contents.second.has_value());
    CHECK(data_hover->contents.second->value ==
          "**data** — *variable*\n\n---\n\n```\nlogic [32-1:0]\n```");

    auto macro_addr_hover = hover_on("macro_addr");
    REQUIRE(macro_addr_hover.has_value());
    REQUIRE(macro_addr_hover->contents.second.has_value());
    CHECK(macro_addr_hover->contents.second->value ==
          "**macro_addr** — *variable*\n\n---\n\n```\nlogic [`WIDTH123:0]\n```");

    auto macro_wdata_hover = hover_on("macro_wdata");
    REQUIRE(macro_wdata_hover.has_value());
    REQUIRE(macro_wdata_hover->contents.second.has_value());
    CHECK(macro_wdata_hover->contents.second->value ==
          "**macro_wdata** — *variable*\n\n---\n\n```\nlogic [`WIDTH-1:0]\n```");

    auto queue_hover = hover_on("queue");
    REQUIRE(queue_hover.has_value());
    REQUIRE(queue_hover->contents.second.has_value());
    CHECK(queue_hover->contents.second->value ==
          "**queue** — *variable*\n\n---\n\n```\nint [4]\n```");

    auto wider_hover = hover_on("wider");
    REQUIRE(wider_hover.has_value());
    REQUIRE(wider_hover->contents.second.has_value());
    CHECK(wider_hover->contents.second->value ==
          "**wider** — *variable*\n\n---\n\n```\nlogic [8-1:0]\n```");

    auto flag_hover = hover_on("flag");
    REQUIRE(flag_hover.has_value());
    REQUIRE(flag_hover->contents.second.has_value());
    CHECK(flag_hover->contents.second->value ==
          "**flag** — *variable*\n\n---\n\n```\nbit\n```");
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

TEST_CASE("hover: shows package parameter values from closed project index", "[hover]") {
    const auto package_path =
        std::filesystem::temp_directory_path() / "lazyverilog_hover_package_param_pkg.sv";
    {
        std::ofstream out(package_path);
        REQUIRE(out.good());
        out << R"SV(package hover_pkg;
    parameter int WIDTH = 32;
endpackage
)SV";
    }

    Analyzer analyzer;
    analyzer.set_extra_files({package_path.string()});
    analyzer.wait_for_background_index_idle();

    const std::string uri = "file:///tmp/lazyverilog_hover_package_param_top.sv";
    const std::string text = R"SV(module top;
    logic [hover_pkg::WIDTH-1:0] data;
endmodule
)SV";
    analyzer.open(uri, text);

    auto [line, col] = lsp_feature_position_of(text, "WIDTH-1");
    lsTextDocumentPositionParams params;
    params.textDocument.uri.raw_uri_ = uri;
    params.position = lsPosition(line, col);

    auto hover = provide_hover(analyzer, params);
    REQUIRE(hover.has_value());
    REQUIRE(hover->contents.second.has_value());
    CHECK(hover->contents.second->value ==
          "**WIDTH** — *parameter*\n\n---\n\n```\nint = 32\n```");

    std::filesystem::remove(package_path);
}

TEST_CASE("hover: evaluates chained package parameter values from closed project index", "[hover]") {
    const auto package_path =
        std::filesystem::temp_directory_path() / "lazyverilog_hover_eval_package_param_pkg.sv";
    {
        std::ofstream out(package_path);
        REQUIRE(out.good());
        out << R"SV(package hover_eval_pkg;
    parameter int N_BITS = 17;
    parameter int B = $clog2(N_BITS);
    parameter int A = B * 2;
endpackage
)SV";
    }

    Analyzer analyzer;
    analyzer.set_extra_files({package_path.string()});
    analyzer.wait_for_background_index_idle();

    const std::string uri = "file:///tmp/lazyverilog_hover_eval_package_param_top.sv";
    const std::string text = R"SV(module top;
    logic [hover_eval_pkg::A-1:0] data;
endmodule
)SV";
    analyzer.open(uri, text);

    auto [line, col] = lsp_feature_position_of(text, "A-1");
    lsTextDocumentPositionParams params;
    params.textDocument.uri.raw_uri_ = uri;
    params.position = lsPosition(line, col);

    auto hover = provide_hover(analyzer, params);
    REQUIRE(hover.has_value());
    REQUIRE(hover->contents.second.has_value());
    CHECK(hover->contents.second->value ==
          "**A** — *parameter*\n\n---\n\n```\nint = B*2\nevaluates to 10\n```");

    std::filesystem::remove(package_path);
}

TEST_CASE("hover: evaluates chained current-file parameter values", "[hover]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/lazyverilog_hover_eval_current_param.sv";
    const std::string text = R"SV(module top;
    parameter int N_BITS = 17;
    parameter int B = $clog2(N_BITS);
    parameter int A = B * 2;
    logic [A-1:0] data;
endmodule
)SV";
    analyzer.open(uri, text);

    auto [line, col] = lsp_feature_position_of(text, "A-1");
    lsTextDocumentPositionParams params;
    params.textDocument.uri.raw_uri_ = uri;
    params.position = lsPosition(line, col);

    auto hover = provide_hover(analyzer, params);
    REQUIRE(hover.has_value());
    REQUIRE(hover->contents.second.has_value());
    CHECK(hover->contents.second->value ==
          "**A** — *parameter*\n\n---\n\n```\nint = B * 2\nevaluates to 10\n```");
}

TEST_CASE("hover: shows package typedef from closed project index", "[hover]") {
    const auto package_path =
        std::filesystem::temp_directory_path() / "lazyverilog_hover_package_type_pkg.sv";
    {
        std::ofstream out(package_path);
        REQUIRE(out.good());
        out << R"SV(package hover_type_pkg;
    typedef logic [7:0] byte_t;
endpackage
)SV";
    }

    Analyzer analyzer;
    analyzer.set_extra_files({package_path.string()});
    analyzer.wait_for_background_index_idle();

    const std::string uri = "file:///tmp/lazyverilog_hover_package_type_top.sv";
    const std::string text = R"SV(module top;
    hover_type_pkg::byte_t data;
endmodule
)SV";
    analyzer.open(uri, text);

    auto [line, col] = lsp_feature_position_of(text, "byte_t data");
    lsTextDocumentPositionParams params;
    params.textDocument.uri.raw_uri_ = uri;
    params.position = lsPosition(line, col);

    auto hover = provide_hover(analyzer, params);
    REQUIRE(hover.has_value());
    REQUIRE(hover->contents.second.has_value());
    CHECK(hover->contents.second->value ==
          "**byte_t** — *typedef*\n\n---\n\n```\nlogic [7:0]\n```");

    std::filesystem::remove(package_path);
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
    analyzer.wait_for_background_index_idle();

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

TEST_CASE("autofunc: incomplete call is cursor-position independent", "[autofunc]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/autofunc_incomplete_call.sv";
    analyzer.open(uri, R"(task add_number(input int a, input int b, output int result);
    result = a + b;
endtask

module top;
    always_comb begin
    add_number(
    end
endmodule
)");

    AutoFuncOptions options;
    options.indent_size = 4;
    options.use_named_arguments = true;

    const std::string expected =
        "add_number(\n"
        "        .a(a),\n"
        "        .b(b),\n"
        "        .result(result)\n"
        "    );";

    for (int col : {4, 5, 13}) {
        auto edit = autofunc(analyzer, uri, 6, col, options);
        REQUIRE(edit.has_value());
        REQUIRE(edit->changes.has_value());
        auto it = edit->changes->find(uri);
        REQUIRE(it != edit->changes->end());
        REQUIRE(it->second.size() == 1);
        CHECK(it->second[0].range.start.line == 6);
        CHECK(it->second[0].range.start.character == 4);
        CHECK(it->second[0].range.end.line == 6);
        CHECK(it->second[0].range.end.character == 15);
        CHECK(it->second[0].newText == expected);
    }
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
    config.format.function_call.arg_count = 3;
    config.format.function_call.layout = "hanging";
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

TEST_CASE("autoarg code action formats full module header", "[autoarg]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/autoarg_code_action_header.sv";
    analyzer.open(uri, R"(
module memory_top();
input i_clk;
input i_rst_n;
input i_data;
input i_data2;
endmodule
)");

    Config config;
    config.format.indent_size = 4;
    config.format.module.non_ansi_port_per_line_enabled = true;
    config.format.module.non_ansi_port_per_line = 3;

    lsCodeActionParams params;
    params.textDocument.uri.raw_uri_ = uri;
    params.range.start = lsPosition(1, 7);

    auto actions = provide_code_actions(analyzer, config, params);
    auto it = std::find_if(actions.begin(), actions.end(), [](const CodeAction& action) {
        return action.title == "AutoArg: generate port list for memory_top";
    });
    REQUIRE(it != actions.end());
    REQUIRE(it->edit.has_value());
    REQUIRE(it->edit->changes.has_value());
    auto change = it->edit->changes->find(uri);
    REQUIRE(change != it->edit->changes->end());
    REQUIRE(change->second.size() == 1);
    CHECK(change->second[0].range.start == lsPosition(1, 0));
    CHECK(change->second[0].newText ==
          "module memory_top(\n"
          "    i_clk, i_rst_n, i_data,\n"
          "    i_data2\n"
          ");");
}

// Cross-file go-to-def fixtures.
// kCrossDefLib is registered as a closed extra file (not opened as a document)
// so all three lookups go through the SyntaxIndex path, not the AST fallback.
static const std::string kCrossDefLib = R"(`define LIB_WIDTH 8

typedef logic [7:0] byte_t;

package util_pkg;
    typedef logic [15:0] word_t;

    function automatic int clamp(input int val, input int lo, input int hi);
        return val < lo ? lo : val > hi ? hi : val;
    endfunction

    task delay_n(input int n);
    endtask
endpackage

module adder #(
    parameter int WIDTH = 8
)(
    input  logic [WIDTH-1:0] i_a,
    input  logic [WIDTH-1:0] i_b,
    output logic [WIDTH-1:0] o_sum
);
    assign o_sum = i_a + i_b;
endmodule
)";

// module top; import util_pkg::*; then instantiate adder and call clamp/delay_n
static const std::string kCrossDefTop = R"(module top;
    import util_pkg::*;
    adder #(.WIDTH(8)) u_adder (.i_a(8'h0), .i_b(8'h0), .o_sum());
    initial begin
        clamp(0, 0, 255);
        delay_n(5);
    end
endmodule
)";

TEST_CASE("definition: module resolves to closed extra file via index", "[definition]") {
    const auto lib_path =
        std::filesystem::temp_directory_path() / "lazyverilog_xdef_lib_module.sv";
    {
        std::ofstream out(lib_path);
        out << kCrossDefLib;
    }

    Analyzer analyzer;
    const std::string top_uri = "file:///tmp/lazyverilog_xdef_top_module.sv";
    analyzer.set_extra_files({lib_path.string()});
    analyzer.wait_for_background_index_idle();
    analyzer.open(top_uri, kCrossDefTop);

    // cursor on "adder" at line 2, col 4
    auto loc = analyzer.definition_of(top_uri, 2, 4);
    REQUIRE(loc.has_value());
    CHECK(loc->uri == "file://" + lib_path.string());
    CHECK(loc->line == 15);
    CHECK(loc->col == 7);

    std::filesystem::remove(lib_path);
}

TEST_CASE("definition: package function resolves to closed extra file via index", "[definition]") {
    const auto lib_path =
        std::filesystem::temp_directory_path() / "lazyverilog_xdef_lib_func.sv";
    {
        std::ofstream out(lib_path);
        out << kCrossDefLib;
    }

    Analyzer analyzer;
    const std::string top_uri = "file:///tmp/lazyverilog_xdef_top_func.sv";
    analyzer.set_extra_files({lib_path.string()});
    analyzer.wait_for_background_index_idle();
    analyzer.open(top_uri, kCrossDefTop);

    // cursor on "clamp" at line 4, col 8
    auto loc = analyzer.definition_of(top_uri, 4, 8);
    REQUIRE(loc.has_value());
    CHECK(loc->uri == "file://" + lib_path.string());
    CHECK(loc->line == 7);
    CHECK(loc->col == 27);

    std::filesystem::remove(lib_path);
}

TEST_CASE("definition: package task resolves to closed extra file via index", "[definition]") {
    const auto lib_path =
        std::filesystem::temp_directory_path() / "lazyverilog_xdef_lib_task.sv";
    {
        std::ofstream out(lib_path);
        out << kCrossDefLib;
    }

    Analyzer analyzer;
    const std::string top_uri = "file:///tmp/lazyverilog_xdef_top_task.sv";
    analyzer.set_extra_files({lib_path.string()});
    analyzer.wait_for_background_index_idle();
    analyzer.open(top_uri, kCrossDefTop);

    // cursor on "delay_n" at line 5, col 8
    auto loc = analyzer.definition_of(top_uri, 5, 8);
    REQUIRE(loc.has_value());
    CHECK(loc->uri == "file://" + lib_path.string());
    CHECK(loc->line == 11);
    CHECK(loc->col == 9);

    std::filesystem::remove(lib_path);
}
