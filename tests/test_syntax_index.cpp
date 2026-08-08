#include "analyzer.hpp"
#include "string_utils.hpp"
#include "syntax_index.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
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

TEST_CASE("syntax_index: finds instantiations nested in generate constructs", "[index]") {
    const std::string text = R"(
module child(input logic clk);
endmodule

module top #(parameter int N = 2) (input logic clk);
    for (genvar i = 0; i < N; i++) begin : gen_loop
        child u_loop (.clk(clk));
    end

    if (N > 1) begin : gen_if
        child u_if (.clk(clk));
    end else begin : gen_else
        child u_else (.clk(clk));
    end

    case (N)
        1: begin : gen_case_one
            child u_case_one (.clk(clk));
        end
        default: begin : gen_case_default
            child u_case_default (.clk(clk));
        end
    endcase

    generate
        begin : gen_region
            child u_region (.clk(clk));
        end
    endgenerate

    for (genvar j = 0; j < N; j++) begin : gen_outer
        if (N > 1) begin : gen_inner
            child u_nested (.clk(clk));
        end
    end
endmodule
)";
    auto tree = slang::syntax::SyntaxTree::fromText(text);
    REQUIRE(tree != nullptr);

    auto idx = SyntaxIndex::build(*tree, text);

    std::set<std::string> names;
    for (const auto& inst : idx.instances) {
        CHECK(inst.module_name == "child");
        CHECK(inst.parent_module == "top");
        names.insert(inst.instance_name);
    }

    CHECK(names == std::set<std::string>{"u_case_default", "u_case_one", "u_else", "u_if",
                                         "u_loop", "u_nested", "u_region"});
}

TEST_CASE("syntax_index: generate-nested instances keep connection metadata", "[index]") {
    const std::string text = R"(
module child(input logic clk, output logic done);
endmodule

module top(input logic clk);
    logic done;
    for (genvar i = 0; i < 2; i++) begin : gen_loop
        child u_loop (
            .clk  (clk),
            .done (done)
        );
    end
endmodule
)";
    auto tree = slang::syntax::SyntaxTree::fromText(text);
    REQUIRE(tree != nullptr);

    auto idx = SyntaxIndex::build(*tree, text);

    REQUIRE(idx.instances.size() == 1);
    const auto& inst = idx.instances.front();
    REQUIRE(inst.connections.size() == 2);
    CHECK(inst.connections[0].port_name == "clk");
    CHECK(inst.connections[0].signal_name == "clk");
    CHECK(inst.connections[1].port_name == "done");
    CHECK(inst.connections[1].signal_name == "done");
    // The instance body spans the connection lines so range-limited features
    // (inlay hints, stale-instance lint) see the whole instantiation.
    CHECK(inst.start_line < inst.connections[0].line - 1);
    CHECK(inst.end_line >= inst.connections[1].line - 1);
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

static const std::string kPackageScoped = R"(
package cpu_pkg;
    parameter  int      WIDTH = 8;
    localparam int      DEPTH = WIDTH * 2;
    typedef logic [7:0] byte_t;
    class packet_cfg;
    endclass
endpackage

module top #(
    parameter int STAGES = 3
);
    localparam int LOCAL_DEPTH = 4;
endmodule
)";

TEST_CASE("syntax_index: package members get scoped-name lookup keys", "[index]") {
    auto tree = slang::syntax::SyntaxTree::fromText(kPackageScoped);
    REQUIRE(tree != nullptr);

    auto idx = SyntaxIndex::build(*tree, kPackageScoped);
    REQUIRE(idx.package_names.contains("cpu_pkg"));

    const auto width_key = package_scoped_key("cpu_pkg", "WIDTH");
    auto value_it = idx.package_value_by_scoped_name.find(width_key);
    REQUIRE(value_it != idx.package_value_by_scoped_name.end());
    REQUIRE(value_it->second < idx.values.size());
    CHECK(idx.values[value_it->second].name == "WIDTH");
    CHECK(idx.values[value_it->second].kind == "parameter");
    CHECK(idx.values[value_it->second].default_value == "8");

    const auto depth_key = package_scoped_key("cpu_pkg", "DEPTH");
    auto depth_it = idx.package_value_by_scoped_name.find(depth_key);
    REQUIRE(depth_it != idx.package_value_by_scoped_name.end());
    CHECK(idx.values[depth_it->second].kind == "localparam");
    CHECK(idx.values[depth_it->second].default_value == "WIDTH*2");

    const auto type_key = package_scoped_key("cpu_pkg", "byte_t");
    auto type_it = idx.package_type_by_scoped_name.find(type_key);
    REQUIRE(type_it != idx.package_type_by_scoped_name.end());
    REQUIRE(type_it->second < idx.typedefs.size());
    CHECK(idx.typedefs[type_it->second].name == "byte_t");
    CHECK(idx.typedefs[type_it->second].resolved == "logic [7:0]");

    const auto class_key = package_scoped_key("cpu_pkg", "packet_cfg");
    auto class_it = idx.package_class_by_scoped_name.find(class_key);
    REQUIRE(class_it != idx.package_class_by_scoped_name.end());
    REQUIRE(class_it->second < idx.classes.size());
    CHECK(idx.classes[class_it->second].name == "packet_cfg");

    // Module members must not leak into the package-scoped maps.
    CHECK_FALSE(idx.package_value_by_scoped_name.contains(package_scoped_key("top", "STAGES")));
}

TEST_CASE("syntax_index: module parameters carry default values", "[index]") {
    auto tree = slang::syntax::SyntaxTree::fromText(kPackageScoped);
    REQUIRE(tree != nullptr);

    auto idx = SyntaxIndex::build(*tree, kPackageScoped);

    auto find_value = [&](std::string_view scope, std::string_view name) -> const ValueEntry* {
        for (const auto& v : idx.values) {
            if (v.parent_scope == scope && v.name == name)
                return &v;
        }
        return nullptr;
    };

    // Header #(...) parameter.
    const auto* stages = find_value("top", "STAGES");
    REQUIRE(stages != nullptr);
    CHECK(stages->kind == "parameter");
    CHECK(stages->default_value == "3");

    // Body parameter -- previously absent from the index entirely.
    const auto* local_depth = find_value("top", "LOCAL_DEPTH");
    REQUIRE(local_depth != nullptr);
    CHECK(local_depth->kind == "localparam");
    CHECK(local_depth->type == "int");
    CHECK(local_depth->default_value == "4");
}

TEST_CASE("syntax_index: package parameters are indexed exactly once", "[index]") {
    auto tree = slang::syntax::SyntaxTree::fromText(kPackageScoped);
    REQUIRE(tree != nullptr);

    auto idx = SyntaxIndex::build(*tree, kPackageScoped);

    // process_package() delegates parameter values to process_module(); a
    // duplicate here would double-count in every linear value scan.
    const auto width_count = std::count_if(idx.values.begin(), idx.values.end(),
                                           [](const ValueEntry& v) {
                                               return v.parent_scope == "cpu_pkg" &&
                                                      v.name == "WIDTH";
                                           });
    CHECK(width_count == 1);
}

TEST_CASE("syntax_index: package members are indexed exactly once", "[index]") {
    // process_package() collects exported symbol names only; process_module()
    // owns every entry.  Processing the same members in both would duplicate
    // each one, which also duplicates the reference occurrences synthesized
    // from index.values.
    const std::string text = R"(package dup_pkg;
    typedef logic [3:0] dup_t;
    typedef enum logic [1:0] { DUP_A, DUP_B } dup_e;
    class dup_c;
    endclass
    function int dup_f(); return 0; endfunction
    logic [7:0] dup_v;
    parameter int DUP_P = 1;
endpackage
)";
    auto tree = slang::syntax::SyntaxTree::fromText(text);
    REQUIRE(tree != nullptr);

    auto idx = SyntaxIndex::build(*tree, text);

    auto count_named = [](const auto& entries, std::string_view name) {
        return std::count_if(entries.begin(), entries.end(),
                             [&](const auto& e) { return e.name == name; });
    };

    CHECK(count_named(idx.typedefs, "dup_t") == 1);
    CHECK(count_named(idx.typedefs, "dup_e") == 1);
    CHECK(count_named(idx.classes, "dup_c") == 1);
    CHECK(count_named(idx.values, "dup_f") == 1);
    CHECK(count_named(idx.values, "dup_v") == 1);
    CHECK(count_named(idx.values, "DUP_P") == 1);

    // Exported-symbol bookkeeping must survive the de-duplication, including
    // enum members, which are not package-body members in their own right.
    const auto& symbols = idx.package_symbols["dup_pkg"];
    auto exported = [&](std::string_view n) {
        return std::find(symbols.begin(), symbols.end(), n) != symbols.end();
    };
    CHECK(exported("dup_t"));
    CHECK(exported("dup_e"));
    CHECK(exported("DUP_A"));
    CHECK(exported("DUP_B"));
    CHECK(exported("dup_c"));
    CHECK(exported("dup_f"));
    CHECK(exported("dup_v"));
    CHECK(exported("DUP_P"));

    // The scoped maps must still resolve every member kind.
    CHECK(idx.package_type_by_scoped_name.contains(package_scoped_key("dup_pkg", "dup_t")));
    CHECK(idx.package_class_by_scoped_name.contains(package_scoped_key("dup_pkg", "dup_c")));
    CHECK(idx.package_value_by_scoped_name.contains(package_scoped_key("dup_pkg", "dup_f")));
    CHECK(idx.package_value_by_scoped_name.contains(package_scoped_key("dup_pkg", "dup_v")));
    CHECK(idx.package_value_by_scoped_name.contains(package_scoped_key("dup_pkg", "DUP_P")));
}

TEST_CASE("syntax_index: qualified package value uses share the declaration SymbolID",
          "[index]") {
    const std::string text = R"(package p1;
    parameter int WIDTH = 8;
endpackage

module top;
    logic [p1::WIDTH-1:0] data;
endmodule
)";
    auto tree = slang::syntax::SyntaxTree::fromText(text);
    REQUIRE(tree != nullptr);

    auto idx = SyntaxIndex::build(*tree, text);

    const auto matching = std::count_if(idx.references.begin(), idx.references.end(),
                                        [](const ReferenceEntry& r) {
                                            return r.name == "WIDTH" &&
                                                   r.symbol_debug == "package_value::p1::WIDTH";
                                        });
    // Distinct source locations carrying the package_value ID: the declaration
    // and the qualified use.  Counting locations rather than raw entries keeps
    // this independent of the pre-existing duplicate occurrence emitted for a
    // package parameter declaration.
    std::set<std::pair<int, int>> locations;
    for (const auto& r : idx.references) {
        if (r.name == "WIDTH" && r.symbol_debug == "package_value::p1::WIDTH")
            locations.emplace(r.line, r.col);
    }
    CHECK(locations.size() == 2);
    CHECK(matching >= 2);

    // No occurrence may fall back to the unresolved form.
    const auto unresolved = std::count_if(idx.references.begin(), idx.references.end(),
                                          [](const ReferenceEntry& r) {
                                              return r.name == "WIDTH" &&
                                                     r.symbol_debug == "name:WIDTH";
                                          });
    CHECK(unresolved == 0);
}

TEST_CASE("syntax_index: merge keeps same-named types from different packages", "[index]") {
    const std::string a_text = R"(package pkg_a;
    typedef logic [3:0] shared_t;
    parameter int SHARED_P = 1;
    class shared_c;
    endclass
endpackage
)";
    const std::string b_text = R"(package pkg_b;
    typedef logic [7:0] shared_t;
    parameter int SHARED_P = 2;
    class shared_c;
    endclass
endpackage
)";

    auto a_tree = slang::syntax::SyntaxTree::fromText(a_text);
    auto b_tree = slang::syntax::SyntaxTree::fromText(b_text);
    REQUIRE(a_tree != nullptr);
    REQUIRE(b_tree != nullptr);

    auto merged = SyntaxIndex::build(*a_tree, a_text);
    merged.merge(SyntaxIndex::build(*b_tree, b_text));

    // The bare-name maps stay first-wins, so they cannot distinguish the two.
    // The scoped maps must resolve each package's own declaration.
    auto type_a = merged.package_type_by_scoped_name.find(package_scoped_key("pkg_a", "shared_t"));
    auto type_b = merged.package_type_by_scoped_name.find(package_scoped_key("pkg_b", "shared_t"));
    REQUIRE(type_a != merged.package_type_by_scoped_name.end());
    REQUIRE(type_b != merged.package_type_by_scoped_name.end());
    CHECK(type_a->second != type_b->second);
    CHECK(merged.typedefs[type_a->second].resolved == "logic [3:0]");
    CHECK(merged.typedefs[type_b->second].resolved == "logic [7:0]");

    auto class_a = merged.package_class_by_scoped_name.find(package_scoped_key("pkg_a", "shared_c"));
    auto class_b = merged.package_class_by_scoped_name.find(package_scoped_key("pkg_b", "shared_c"));
    REQUIRE(class_a != merged.package_class_by_scoped_name.end());
    REQUIRE(class_b != merged.package_class_by_scoped_name.end());
    CHECK(class_a->second != class_b->second);
    CHECK(merged.classes[class_a->second].parent_scope == "pkg_a");
    CHECK(merged.classes[class_b->second].parent_scope == "pkg_b");

    auto value_a = merged.package_value_by_scoped_name.find(package_scoped_key("pkg_a", "SHARED_P"));
    auto value_b = merged.package_value_by_scoped_name.find(package_scoped_key("pkg_b", "SHARED_P"));
    REQUIRE(value_a != merged.package_value_by_scoped_name.end());
    REQUIRE(value_b != merged.package_value_by_scoped_name.end());
    CHECK(merged.values[value_a->second].default_value == "1");
    CHECK(merged.values[value_b->second].default_value == "2");
}

TEST_CASE("header text cache: a new generation drops previously cached text", "[index]") {
    // Every path that invalidates parse results bumps the background generation:
    // config reload, a changed header, a changed open buffer.  Cached text from
    // an older generation must never be handed to a parse running under a newer
    // one, or a shard would be built from text the edit already replaced.
    HeaderTextCache cache;
    cache.note_parse(1);
    cache.store(1, "/proj/shared.svh", "localparam int W = 8;\n");
    REQUIRE(cache.seed_candidates(1).size() == 1);

    auto after_bump = cache.seed_candidates(2);
    CHECK(after_bump.empty());

    // The cache is usable again under the new generation.
    cache.note_parse(2);
    cache.store(2, "/proj/shared.svh", "localparam int W = 32;\n");
    auto refilled = cache.seed_candidates(2);
    REQUIRE(refilled.size() == 1);
    CHECK(*refilled.front().second == "localparam int W = 32;\n");
}

TEST_CASE("header text cache: only widely shared headers are offered for seeding", "[index]") {
    // Seeding copies text into the target SourceManager, so it pays off only for
    // headers the next parse is likely to include.  A header every file pulls in
    // stays on offer; one that a single file of many pulled in drops off, which
    // is what keeps a design with hundreds of distinct headers from copying all
    // of them into every file's SourceManager.
    HeaderTextCache cache;
    for (int parse = 0; parse < 10; ++parse) {
        cache.note_parse(1);
        cache.store(1, "/proj/common.svh", "localparam int W = 8;\n");
        if (parse == 0)
            cache.store(1, "/proj/onlyone.svh", "localparam int X = 1;\n");
    }

    const auto candidates = cache.seed_candidates(1);
    REQUIRE(candidates.size() == 1);
    CHECK(candidates.front().first == "/proj/common.svh");
}

TEST_CASE("project index: shared header text is indexed once per including file", "[index]") {
    // Background indexing seeds already-known header text into each file's
    // SourceManager so a shared header is not re-read once per including file.
    // Seeding must not change what lands in a shard, and unsaved editor text
    // must still win over anything the cache holds.
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() / "lazyverilog_header_text_cache";
    fs::remove_all(dir);
    fs::create_directories(dir);

    const auto header_path = dir / "shared.svh";
    const auto first_path = dir / "first.sv";
    const auto second_path = dir / "second.sv";

    auto write_file = [](const fs::path& path, const std::string& text) {
        std::ofstream out(path);
        REQUIRE(out.good());
        out << text;
    };

    auto shared_width = [](const ProjectIndexSnapshot& snapshot) -> std::string {
        // Every shard carries the header's declarations, so any shard answers.
        for (const auto& shard : snapshot.shards) {
            if (!shard.index)
                continue;
            for (const auto& value : shard.index->values) {
                if (value.name == "SHARED_W")
                    return value.default_value;
            }
        }
        return {};
    };

    write_file(header_path, "package shared_pkg;\n    localparam int SHARED_W = 8;\nendpackage\n");
    write_file(first_path, "`include \"shared.svh\"\n"
                           "module first;\n    logic [shared_pkg::SHARED_W-1:0] a;\nendmodule\n");
    write_file(second_path, "`include \"shared.svh\"\n"
                            "module second;\n    logic [shared_pkg::SHARED_W-1:0] b;\nendmodule\n");

    Analyzer analyzer;
    analyzer.set_project_index_publish_debounce_ms(0);
    analyzer.set_include_dirs({dir.string()});
    analyzer.set_extra_files({first_path.string(), second_path.string()});
    analyzer.wait_for_background_index_idle();

    // Both files include the same header, so whichever parses second in the
    // burst is served from the cache rather than from disk.  It must still see
    // the header's declarations.
    const auto header_uri = uri_from_path(header_path);
    auto snapshot = analyzer.project_index_snapshot();
    REQUIRE(snapshot);
    REQUIRE(snapshot->shards.size() == 2);
    for (const auto& shard : snapshot->shards) {
        REQUIRE(shard.index);
        CHECK(std::find(shard.index->include_dependencies.begin(),
                        shard.index->include_dependencies.end(),
                        header_uri) != shard.index->include_dependencies.end());
    }
    CHECK(shared_width(*snapshot) == "8");

    // Editing the header through an open buffer must reach both dependents.
    // Cached disk text may never shadow the unsaved overlay.
    analyzer.open(header_uri, "package shared_pkg;\n    localparam int SHARED_W = 8;\nendpackage\n");
    analyzer.change(header_uri,
                    "package shared_pkg;\n    localparam int SHARED_W = 32;\nendpackage\n");
    analyzer.wait_for_background_index_idle();

    auto reparsed = analyzer.project_index_snapshot();
    REQUIRE(reparsed);
    REQUIRE(reparsed->shards.size() == 2);
    CHECK(shared_width(*reparsed) == "32");

    fs::remove_all(dir);
}
