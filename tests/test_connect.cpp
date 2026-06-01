#include "analyzer.hpp"
#include "features/connect.hpp"
#include <catch2/catch_test_macros.hpp>

TEST_CASE("connect: reports modules, ports, and hierarchical instances", "[connect]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/connect_fixture.sv";
    analyzer.open(uri, R"(
module producer(output logic [7:0] data);
endmodule
module consumer(input logic [7:0] data);
endmodule
module top;
    producer u_prod (.data());
    consumer u_cons (.data());
endmodule
)");

    const auto json = connect_info_json(analyzer, uri);
    CHECK(json.find("\"producer\"") != std::string::npos);
    CHECK(json.find("\"consumer\"") != std::string::npos);
    CHECK(json.find("top.u_prod") != std::string::npos);
    CHECK(json.find("top.u_cons") != std::string::npos);
}

TEST_CASE("connect: preview and apply produce wiring edits", "[connect]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/connect_apply_fixture.sv";
    analyzer.open(uri, R"(
module producer(output logic [7:0] data);
endmodule
module consumer(input logic [7:0] data);
endmodule
module top;
    producer u_prod (.data());
    consumer u_cons (.data());
endmodule
)");

    const auto preview = connect_apply_preview_json(analyzer, uri, "top.u_prod", "data",
                                                    "top.u_cons", "data", "data_w");
    CHECK(preview.find("connect u_prod.data(data_w)") != std::string::npos);
    CHECK(preview.find("declare logic [7:0] data_w in top") != std::string::npos);

    const auto edit = connect_apply_edit_json(analyzer, uri, "top.u_prod", "data",
                                              "top.u_cons", "data", "data_w");
    CHECK(edit.find(".data(data_w)") != std::string::npos);
    CHECK(edit.find("logic [7:0] data_w;") != std::string::npos);
}

TEST_CASE("interface: returns shared signal rows and connect edits", "[connect][interface]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/interface_fixture.sv";
    analyzer.open(uri, R"(
module producer(output logic ready);
endmodule
module consumer(input logic ready);
endmodule
module top;
    logic ready_w;
    producer u_prod (.ready(ready_w));
    consumer u_cons (.ready(ready_w));
endmodule
)");

    const auto iface = interface_json(analyzer, uri, "u_prod", "u_cons");
    CHECK(iface.find("\"inst1_port\":\"ready\"") != std::string::npos);
    CHECK(iface.find("\"inst2_port\":\"ready\"") != std::string::npos);
    CHECK(iface.find("ready_w") != std::string::npos);

    const auto single = single_interface_json(analyzer, uri, "u_prod");
    CHECK(single.find("\"other_inst\":\"u_cons\"") != std::string::npos);

    const auto edit = interface_connect_edit_json(analyzer, uri, "u_prod", "u_cons",
                                                  "ready", "ready", "ready2_w", "logic");
    CHECK(edit.find(".ready(ready2_w)") != std::string::npos);
    CHECK(edit.find("logic ready2_w;") != std::string::npos);
}

TEST_CASE("interface: declares complete net port types and preserves symbolic dimensions", "[connect][interface]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/interface_type_fixture.sv";
    analyzer.open(uri, R"(
`define BUS_W 6
typedef struct packed { logic valid; } payload_t;
module memory(input wire [5:0] i_data, output wire [5:0] o_data);
endmodule
module typed_source(output payload_t [`BUS_W-1:0] payload);
endmodule
module typed_sink(input payload_t [DEPTH-1:0] payload);
endmodule
module top;
    memory u_mem2 (.i_data(), .o_data());
    memory u_mem3 (.i_data(), .o_data());
    typed_source u_src (.payload());
    typed_sink u_dst (.payload());
endmodule
)");

    const auto iface = interface_json(analyzer, uri, "u_mem2", "u_mem3");
    CHECK(iface.find("\"type\":\"wire [5:0]\"") != std::string::npos);

    const auto net_edit = interface_connect_edit_json(analyzer, uri, "u_mem2", "u_mem3",
                                                      "o_data", "i_data", "data32", "wire [5:0]");
    CHECK(net_edit.find("logic [5:0] data32;") != std::string::npos);

    const auto reverse_order_edit = interface_connect_edit_json(analyzer, uri, "u_mem3", "u_mem2",
                                                               "i_data", "o_data", "data33", "wire [5:0]");
    CHECK(reverse_order_edit.find("logic [5:0] data33;") != std::string::npos);

    const auto typed = interface_json(analyzer, uri, "u_src", "u_dst");
    CHECK(typed.find("payload_t [`BUS_W-1:0]") != std::string::npos);
    CHECK(typed.find("payload_t [DEPTH-1:0]") != std::string::npos);

    const auto typed_edit = interface_connect_edit_json(analyzer, uri, "u_src", "u_dst",
                                                        "payload", "payload", "payload_w",
                                                        "payload_t [`BUS_W-1:0]");
    CHECK(typed_edit.find("payload_t [`BUS_W-1:0] payload_w;") != std::string::npos);
}

TEST_CASE("interface: falls back when UI supplies only packed dimensions", "[connect][interface]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/interface_dimension_only_fixture.sv";
    analyzer.open(uri, R"(
module producer(output wire [WIDTH-1:0] data);
endmodule
module consumer(input wire [WIDTH-1:0] data);
endmodule
module top;
    producer u_prod (.data());
    consumer u_cons (.data());
endmodule
)");

    const auto edit = interface_connect_edit_json(analyzer, uri, "u_prod", "u_cons",
                                                  "data", "data", "data_w", "[WIDTH-1:0]");
    CHECK(edit.find("logic [WIDTH-1:0] data_w;") != std::string::npos);
}
