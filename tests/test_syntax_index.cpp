#include "analyzer.hpp"
#include "syntax_index.hpp"
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <slang/syntax/SyntaxTree.h>

static const std::string kMemory = R"(
module memory #(parameter WIDTH=8, DEPTH=256) (
    input  logic        clk,
    input  logic        we,
    input  logic [7:0]  addr,
    input  logic [7:0]  din,
    output logic [7:0]  dout
);
    logic [WIDTH-1:0] mem [0:DEPTH-1];
    always_ff @(posedge clk) begin
        if (we) mem[addr] <= din;
        dout <= mem[addr];
    end
endmodule

module top (
    input logic clk
);
    memory u_mem (
        .clk(clk), .we(1'b0), .addr(8'h0), .din(8'h0), .dout()
    );
endmodule
)";

TEST_CASE("syntax_index: finds memory module and its ports", "[index]") {
    auto tree = slang::syntax::SyntaxTree::fromText(kMemory);
    REQUIRE(tree != nullptr);

    auto idx = SyntaxIndex::build(*tree, kMemory);

    auto it = std::find_if(idx.modules.begin(), idx.modules.end(),
                           [](const ModuleEntry& m) { return m.name == "memory"; });
    REQUIRE(it != idx.modules.end());
    REQUIRE(idx.module_by_name.contains("memory"));
    CHECK(idx.modules[idx.module_by_name.at("memory")].name == "memory");
    CHECK(it->line > 0);
    CHECK(it->ports.size() >= 5);

    auto port_it = std::find_if(it->ports.begin(), it->ports.end(),
                                [](const PortEntry& p) { return p.name == "clk"; });
    REQUIRE(port_it != it->ports.end());
    REQUIRE(it->port_by_name.contains("clk"));
    CHECK(it->ports[it->port_by_name.at("clk")].name == "clk");
    CHECK(port_it->direction == "input");
}

TEST_CASE("syntax_index: finds top module", "[index]") {
    auto tree = slang::syntax::SyntaxTree::fromText(kMemory);
    auto idx = SyntaxIndex::build(*tree, kMemory);

    auto it = std::find_if(idx.modules.begin(), idx.modules.end(),
                           [](const ModuleEntry& m) { return m.name == "top"; });
    REQUIRE(it != idx.modules.end());
}

TEST_CASE("syntax_index: finds memory instantiation in top", "[index]") {
    auto tree = slang::syntax::SyntaxTree::fromText(kMemory);
    auto idx = SyntaxIndex::build(*tree, kMemory);

    auto it = std::find_if(idx.instances.begin(), idx.instances.end(),
                           [](const InstanceEntry& e) { return e.module_name == "memory"; });
    REQUIRE(it != idx.instances.end());
    CHECK(it->instance_name == "u_mem");
    CHECK(it->parent_module == "top");
}

TEST_CASE("syntax_index: reference occurrences carry owner-qualified symbol IDs", "[index]") {
    const std::string text = R"(
module memory #(parameter WIDTH = 8) (input logic clk);
endmodule

module uart #(parameter WIDTH = 8) (input logic clk);
endmodule

module top(input logic clk);
    memory #(.WIDTH(16)) u_mem(.clk(clk));
    uart   #(.WIDTH(32)) u_uart(.clk(clk));
endmodule
)";
    auto tree = slang::syntax::SyntaxTree::fromText(text);
    REQUIRE(tree != nullptr);

    auto idx = SyntaxIndex::build(*tree, text);

    auto has_ref = [&](std::string_view name, std::string_view symbol_id) {
        return std::find_if(idx.references.begin(), idx.references.end(), [&](const ReferenceEntry& ref) {
                   return ref.name == name && ref.symbol_debug == symbol_id &&
                          ref.symbol_id == SymbolID::from_canonical(symbol_id);
               }) != idx.references.end();
    };

    // Same spelling, different owner.  Closed-file references can now match
    // memory.clk without also returning uart.clk.
    CHECK(has_ref("clk", "module_port::memory::clk"));
    CHECK(has_ref("clk", "module_port::uart::clk"));
    CHECK(has_ref("WIDTH", "module_param::memory::WIDTH"));
    CHECK(has_ref("WIDTH", "module_param::uart::WIDTH"));

    // Module declarations and hierarchy type uses also share a module-level
    // identity instead of relying on plain text.
    CHECK(has_ref("memory", "module::memory"));
    CHECK(has_ref("uart", "module::uart"));
}

TEST_CASE("syntax_index: symbol IDs include module, package, class, and typedef scopes",
          "[index]") {
    const std::string text = R"(
package p1;
    parameter int WIDTH = 8;
    typedef logic [3:0] data_t;
    class packet;
        bit valid;
        function bit is_valid();
            return valid;
        endfunction
    endclass
endpackage

module top;
    logic state;
    assign state = 1'b0;
    data_t d;
endmodule
)";
    auto tree = slang::syntax::SyntaxTree::fromText(text);
    REQUIRE(tree != nullptr);

    auto idx = SyntaxIndex::build(*tree, text);

    auto has_ref = [&](std::string_view name, std::string_view symbol_id) {
        return std::find_if(idx.references.begin(), idx.references.end(), [&](const ReferenceEntry& ref) {
                   return ref.name == name && ref.symbol_debug == symbol_id &&
                          ref.symbol_id == SymbolID::from_canonical(symbol_id);
               }) != idx.references.end();
    };

    CHECK(has_ref("WIDTH", "package_value::p1::WIDTH"));
    CHECK(has_ref("data_t", "typedef::p1::data_t"));
    CHECK(has_ref("packet", "class::p1::packet"));
    CHECK(has_ref("valid", "class_field::p1::packet::valid"));
    CHECK(has_ref("state", "module_signal::top::state"));
}

TEST_CASE("syntax_index: typedef struct fields have typedef-scoped SymbolIDs", "[index]") {
    const std::string text = R"(
typedef struct {
    logic [7:0] addr;
    logic       valid;
} packet_wo_data_t;

typedef struct {
    logic signed [7:0] addr;
    logic        [31:0] data;
    logic               valid;
} packet_ta;
)";
    auto tree = slang::syntax::SyntaxTree::fromText(text);
    REQUIRE(tree != nullptr);

    auto idx = SyntaxIndex::build(*tree, text);

    auto has_ref = [&](std::string_view name, std::string_view symbol_id) {
        return std::find_if(idx.references.begin(), idx.references.end(), [&](const ReferenceEntry& ref) {
                   return ref.name == name && ref.symbol_debug == symbol_id &&
                          ref.symbol_id == SymbolID::from_canonical(symbol_id);
               }) != idx.references.end();
    };

    CHECK(has_ref("addr", "typedef_field::packet_wo_data_t::addr"));
    CHECK(has_ref("addr", "typedef_field::packet_ta::addr"));
    CHECK(has_ref("valid", "typedef_field::packet_wo_data_t::valid"));
    CHECK(has_ref("valid", "typedef_field::packet_ta::valid"));
    CHECK(SymbolID::from_canonical("typedef_field::packet_wo_data_t::addr") !=
          SymbolID::from_canonical("typedef_field::packet_ta::addr"));
}

TEST_CASE("syntax_index: SymbolID separates same names in different semantic scopes",
          "[index]") {
    const std::string text = R"(
package p1;
    parameter int WIDTH = 8;
    typedef logic [7:0] data_t;
    class packet;
        bit valid;
    endclass
endpackage

package p2;
    parameter int WIDTH = 16;
    typedef logic [15:0] data_t;
    class packet;
        bit valid;
    endclass
endpackage

module top_a;
    logic state;
    assign state = 1'b0;
endmodule

module top_b;
    logic state;
    assign state = 1'b1;
endmodule
)";
    auto tree = slang::syntax::SyntaxTree::fromText(text);
    REQUIRE(tree != nullptr);

    auto idx = SyntaxIndex::build(*tree, text);

    auto count_refs = [&](std::string_view name, std::string_view canonical) {
        return std::count_if(idx.references.begin(), idx.references.end(),
                             [&](const ReferenceEntry& ref) {
                                 return ref.name == name && ref.symbol_debug == canonical &&
                                        ref.symbol_id == SymbolID::from_canonical(canonical);
                             });
    };

    // Same spelling, different package.
    CHECK(count_refs("WIDTH", "package_value::p1::WIDTH") >= 1);
    CHECK(count_refs("WIDTH", "package_value::p2::WIDTH") >= 1);
    CHECK(SymbolID::from_canonical("package_value::p1::WIDTH") !=
          SymbolID::from_canonical("package_value::p2::WIDTH"));

    // Same typedef and class names, different package scopes.
    CHECK(count_refs("data_t", "typedef::p1::data_t") >= 1);
    CHECK(count_refs("data_t", "typedef::p2::data_t") >= 1);
    CHECK(count_refs("packet", "class::p1::packet") >= 1);
    CHECK(count_refs("packet", "class::p2::packet") >= 1);

    // Same field name, same class name, different package-qualified classes.
    CHECK(count_refs("valid", "class_field::p1::packet::valid") >= 1);
    CHECK(count_refs("valid", "class_field::p2::packet::valid") >= 1);
    CHECK(SymbolID::from_canonical("class_field::p1::packet::valid") !=
          SymbolID::from_canonical("class_field::p2::packet::valid"));

    // Same module signal name, different module scopes.
    CHECK(count_refs("state", "module_signal::top_a::state") >= 2);
    CHECK(count_refs("state", "module_signal::top_b::state") >= 2);
    CHECK(SymbolID::from_canonical("module_signal::top_a::state") !=
          SymbolID::from_canonical("module_signal::top_b::state"));
}

TEST_CASE("syntax_index: ambiguous type names do not get unique use-site SymbolIDs",
          "[index]") {
    const std::string text = R"(
package p1;
    typedef logic [7:0] data_t;
endpackage

package p2;
    typedef logic [15:0] data_t;
endpackage

module top;
    data_t value;
endmodule
)";
    auto tree = slang::syntax::SyntaxTree::fromText(text);
    REQUIRE(tree != nullptr);

    auto idx = SyntaxIndex::build(*tree, text);

    const auto p1_count = std::count_if(
        idx.references.begin(), idx.references.end(), [](const ReferenceEntry& ref) {
            return ref.name == "data_t" && ref.symbol_debug == "typedef::p1::data_t";
        });
    const auto p2_count = std::count_if(
        idx.references.begin(), idx.references.end(), [](const ReferenceEntry& ref) {
            return ref.name == "data_t" && ref.symbol_debug == "typedef::p2::data_t";
        });
    const auto fallback_count = std::count_if(
        idx.references.begin(), idx.references.end(), [](const ReferenceEntry& ref) {
            return ref.name == "data_t" && ref.symbol_debug == "name:data_t";
        });

    // Each typedef declaration itself is identified, but the ambiguous use in
    // `module top` is deliberately left unresolved instead of choosing p1 or p2
    // by name only.
    CHECK(p1_count >= 1);
    CHECK(p2_count >= 1);
    CHECK(fallback_count >= 1);
}

TEST_CASE("syntax_index: standalone package root preserves package symbols", "[index]") {
    const std::string text =
        "package standalone_pkg;\n"
        "    typedef enum { PKG_IDLE, PKG_DONE } pkg_state_t;\n"
        "    parameter int PKG_WIDTH = 8;\n"
        "endpackage\n";
    auto tree = slang::syntax::SyntaxTree::fromText(text);
    REQUIRE(tree != nullptr);

    auto idx = SyntaxIndex::build(*tree, text);

    CHECK(idx.package_names.contains("standalone_pkg"));
    REQUIRE(idx.package_symbols.contains("standalone_pkg"));
    const auto& symbols = idx.package_symbols.at("standalone_pkg");
    CHECK(std::find(symbols.begin(), symbols.end(), "pkg_state_t") != symbols.end());
    CHECK(std::find(symbols.begin(), symbols.end(), "PKG_IDLE") != symbols.end());
    CHECK(std::find(symbols.begin(), symbols.end(), "PKG_WIDTH") != symbols.end());
}

TEST_CASE("syntax_index: standalone interface root preserves interface identity", "[index]") {
    const std::string text =
        "interface standalone_if;\n"
        "    logic req;\n"
        "    modport master(output req);\n"
        "endinterface\n";
    auto tree = slang::syntax::SyntaxTree::fromText(text);
    REQUIRE(tree != nullptr);

    auto idx = SyntaxIndex::build(*tree, text);

    CHECK(idx.interface_names.contains("standalone_if"));
    REQUIRE(idx.module_by_name.contains("standalone_if"));
    const auto& entry = idx.modules.at(idx.module_by_name.at("standalone_if"));
    CHECK(entry.name == "standalone_if");
    CHECK(entry.modports.size() == 1);
    CHECK(entry.modports[0].name == "master");
}

TEST_CASE("syntax_index: standalone class and typedef roots are indexed", "[index]") {
    {
        const std::string text =
            "class standalone_cfg;\n"
            "    int timeout;\n"
            "endclass\n";
        auto tree = slang::syntax::SyntaxTree::fromText(text);
        REQUIRE(tree != nullptr);

        auto idx = SyntaxIndex::build(*tree, text);
        CHECK(idx.class_by_name.contains("standalone_cfg"));
    }

    {
        const std::string text =
            "typedef struct packed {\n"
            "    logic valid;\n"
            "} standalone_packet_t;\n";
        auto tree = slang::syntax::SyntaxTree::fromText(text);
        REQUIRE(tree != nullptr);

        auto idx = SyntaxIndex::build(*tree, text);
        CHECK(idx.typedef_by_name.contains("standalone_packet_t"));
    }
}
