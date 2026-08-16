#include "analyzer.hpp"
#include "features/document_symbols.hpp"
#include <catch2/catch_test_macros.hpp>
#include <algorithm>

static lsDocumentSymbolParams make_params(const std::string& uri) {
    lsDocumentSymbolParams p;
    p.textDocument.uri.raw_uri_ = uri;
    return p;
}

static const lsDocumentSymbol* find_child(const std::vector<lsDocumentSymbol>& symbols,
                                          const std::string& name) {
    auto it = std::find_if(symbols.begin(), symbols.end(),
                           [&](const lsDocumentSymbol& s) { return s.name == name; });
    return it == symbols.end() ? nullptr : &*it;
}

static std::vector<lsDocumentSymbol> children_of(const lsDocumentSymbol& sym) {
    return sym.children ? *sym.children : std::vector<lsDocumentSymbol>{};
}

TEST_CASE("documentSymbol: module lists ports as children", "[documentSymbol]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/docsym_module.sv";
    analyzer.open(uri, "module top(\n"
                       "    input  logic clk,\n"
                       "    output logic done\n"
                       ");\n"
                       "endmodule\n");

    auto result = provide_document_symbols(analyzer, make_params(uri));
    const auto* top = find_child(result, "top");
    REQUIRE(top != nullptr);
    CHECK(top->kind == lsSymbolKind::Module);

    const auto children = children_of(*top);
    CHECK(find_child(children, "clk") != nullptr);
    CHECK(find_child(children, "done") != nullptr);
}

TEST_CASE("documentSymbol: class lists fields and methods", "[documentSymbol]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/docsym_class.sv";
    analyzer.open(uri, "class driver;\n"
                       "    int unsigned depth;\n"
                       "    function void run();\n"
                       "    endfunction\n"
                       "    task drive();\n"
                       "    endtask\n"
                       "endclass\n");

    auto result = provide_document_symbols(analyzer, make_params(uri));
    const auto* cls = find_child(result, "driver");
    REQUIRE(cls != nullptr);
    CHECK(cls->kind == lsSymbolKind::Class);

    const auto children = children_of(*cls);
    const auto* field = find_child(children, "depth");
    REQUIRE(field != nullptr);
    CHECK(field->kind == lsSymbolKind::Field);

    const auto* fn = find_child(children, "run");
    REQUIRE(fn != nullptr);
    CHECK(fn->kind == lsSymbolKind::Function);

    const auto* task = find_child(children, "drive");
    REQUIRE(task != nullptr);
    CHECK(task->kind == lsSymbolKind::Method);
}

TEST_CASE("documentSymbol: a class declared in a package nests under the package",
          "[documentSymbol]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/docsym_package.sv";
    analyzer.open(uri, "package cfg_pkg;\n"
                       "    class base_cfg;\n"
                       "        int depth;\n"
                       "    endclass\n"
                       "endpackage\n");

    auto result = provide_document_symbols(analyzer, make_params(uri));
    const auto* pkg = find_child(result, "cfg_pkg");
    REQUIRE(pkg != nullptr);
    CHECK(pkg->kind == lsSymbolKind::Package);
    // The nested class must not also appear as a stray top-level symbol.
    CHECK(find_child(result, "base_cfg") == nullptr);

    const auto children = children_of(*pkg);
    const auto* cls = find_child(children, "base_cfg");
    REQUIRE(cls != nullptr);
    CHECK(cls->kind == lsSymbolKind::Class);
}

TEST_CASE("documentSymbol: interface lists signals and modports", "[documentSymbol]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/docsym_interface.sv";
    analyzer.open(uri, "interface bus_if;\n"
                       "    logic valid;\n"
                       "    logic ready;\n"
                       "    modport master(output valid, input ready);\n"
                       "endinterface\n");

    auto result = provide_document_symbols(analyzer, make_params(uri));
    const auto* iface = find_child(result, "bus_if");
    REQUIRE(iface != nullptr);
    CHECK(iface->kind == lsSymbolKind::Interface);

    const auto children = children_of(*iface);
    CHECK(find_child(children, "valid") != nullptr);
    CHECK(find_child(children, "ready") != nullptr);
    const auto* modport = find_child(children, "master");
    REQUIRE(modport != nullptr);
    CHECK(modport->kind == lsSymbolKind::Interface);
}

TEST_CASE("documentSymbol: enum typedef lists its members", "[documentSymbol]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/docsym_enum.sv";
    analyzer.open(uri, "typedef enum { IDLE, BUSY, DONE } state_t;\n"
                       "module top;\n"
                       "endmodule\n");

    auto result = provide_document_symbols(analyzer, make_params(uri));
    const auto* en = find_child(result, "state_t");
    REQUIRE(en != nullptr);
    CHECK(en->kind == lsSymbolKind::Enum);

    const auto children = children_of(*en);
    CHECK(find_child(children, "IDLE") != nullptr);
    CHECK(find_child(children, "BUSY") != nullptr);
    CHECK(find_child(children, "DONE") != nullptr);
}

TEST_CASE("documentSymbol: a module instance appears as a child of its parent module",
          "[documentSymbol]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/docsym_instance.sv";
    analyzer.open(uri, "module sub;\n"
                       "endmodule\n"
                       "module top;\n"
                       "    sub u_sub();\n"
                       "endmodule\n");

    auto result = provide_document_symbols(analyzer, make_params(uri));
    const auto* top = find_child(result, "top");
    REQUIRE(top != nullptr);

    const auto children = children_of(*top);
    const auto* inst = find_child(children, "u_sub");
    REQUIRE(inst != nullptr);
    CHECK(inst->kind == lsSymbolKind::Object);
}
