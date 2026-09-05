#include "analyzer.hpp"
#include "features/document_symbols.hpp"
#include "string_utils.hpp"
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <functional>
#include <filesystem>
#include <fstream>

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

static std::filesystem::path write_temp_sv(const std::string& name, const std::string& text) {
    auto path = std::filesystem::temp_directory_path() / name;
    std::ofstream out(path);
    REQUIRE(out.good());
    out << text;
    out.close();
    return path;
}

static void collect_lines(const std::vector<lsDocumentSymbol>& symbols, std::vector<int>& lines) {
    for (const auto& s : symbols) {
        lines.push_back(s.range.start.line);
        lines.push_back(s.range.end.line);
        if (s.children)
            collect_lines(*s.children, lines);
    }
}

TEST_CASE("documentSymbol: every reported range lies inside the requested document",
          "[documentSymbol]") {
    Analyzer analyzer;
    // lsDocumentSymbol carries no URI, so every range it reports is read
    // against the requested document.  A symbol declared in an `include`d
    // header therefore must not be reported here under the header's own line
    // numbers: those land past the end of the file the client asked about.
    // The padding puts the header's class well beyond this file's length.
    std::string header;
    for (int i = 0; i < 40; ++i)
        header += "// padding\n";
    header += "class header_cls;\n"
              "    int header_field;\n"
              "endclass\n";
    write_temp_sv("docsym_foreign_header.svh", header);
    const std::string top_text = "`include \"docsym_foreign_header.svh\"\n"
                                 "class local_cls;\n"
                                 "    int local_field;\n"
                                 "    function void own_fn();\n"
                                 "    endfunction\n"
                                 "endclass\n";
    const auto top_path = write_temp_sv("docsym_foreign_top.sv", top_text);
    const std::string top_uri = uri_from_path(top_path);
    analyzer.open(top_uri, top_text);

    auto result = provide_document_symbols(analyzer, make_params(top_uri));

    const int line_count = 6; // lines in docsym_foreign_top.sv
    std::vector<int> lines;
    collect_lines(result, lines);
    REQUIRE(!lines.empty());
    for (int line : lines)
        CHECK(line < line_count);

    // The class declared here survives, along with its hand-written members.
    const auto* local = find_child(result, "local_cls");
    REQUIRE(local != nullptr);
    const auto members = children_of(*local);
    CHECK(find_child(members, "local_field") != nullptr);
    CHECK(find_child(members, "own_fn") != nullptr);

    // The header's own class belongs to the header's document, not this one.
    CHECK(find_child(result, "header_cls") == nullptr);
}

// Every kind an outline reports must be one the LSP spec defines (1..26).
// lspcpp carries extensions past that range (`TypeAlias = 252`), and a client
// that validates the enum drops the symbol instead of drawing it.
TEST_CASE("documentSymbol: every emitted kind is within the LSP SymbolKind range",
          "[documentSymbol]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/docsym_kind_range.sv";
    analyzer.open(uri, "package p;\n"
                       "    typedef logic [7:0] byte_t;\n"
                       "    typedef struct packed { logic a; } pair_t;\n"
                       "    typedef enum logic { E0, E1 } mode_e;\n"
                       "endpackage\n");

    auto result = provide_document_symbols(analyzer, make_params(uri));
    REQUIRE_FALSE(result.empty());

    std::vector<const lsDocumentSymbol*> stack;
    for (const auto& sym : result)
        stack.push_back(&sym);
    while (!stack.empty()) {
        const auto* sym = stack.back();
        stack.pop_back();
        CAPTURE(sym->name);
        CHECK((int)sym->kind >= 1);
        CHECK((int)sym->kind <= 26);
        if (sym->children)
            for (const auto& child : *sym->children)
                stack.push_back(&child);
    }
}

// A typedef declared inside a class used to land next to the package instead of
// under the class, because the container map held only modules and packages.

// A typedef declared inside a class used to land next to the package instead of
// under the class, because the container map held only modules and packages.
TEST_CASE("documentSymbol: a class-scoped typedef nests under its class", "[documentSymbol]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/docsym_class_typedef.sv";
    analyzer.open(uri, "package p;\n"
                       "    class box_c;\n"
                       "        typedef int elem_t;\n"
                       "        int store;\n"
                       "    endclass\n"
                       "endpackage\n");

    auto result = provide_document_symbols(analyzer, make_params(uri));
    CHECK(find_child(result, "elem_t") == nullptr);

    const auto* pkg = find_child(result, "p");
    REQUIRE(pkg != nullptr);
    // children_of() returns by value, so hold each level in a named vector
    // rather than pointing into a temporary.
    const auto pkg_children = children_of(*pkg);
    const auto* cls = find_child(pkg_children, "box_c");
    REQUIRE(cls != nullptr);
    const auto cls_children = children_of(*cls);
    CHECK(find_child(cls_children, "elem_t") != nullptr);
}

// Body localparams never appeared in the outline; only parameter *ports* did.

// Body localparams never appeared in the outline; only parameter *ports* did.
TEST_CASE("documentSymbol: module localparams appear as constants", "[documentSymbol]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/docsym_localparam.sv";
    analyzer.open(uri, "module top #(parameter int W = 4) ();\n"
                       "    localparam int DEPTH = 8;\n"
                       "    logic [W-1:0] data;\n"
                       "endmodule\n");

    auto result = provide_document_symbols(analyzer, make_params(uri));
    const auto* top = find_child(result, "top");
    REQUIRE(top != nullptr);

    const auto children = children_of(*top);
    const auto* depth = find_child(children, "DEPTH");
    REQUIRE(depth != nullptr);
    CHECK(depth->kind == lsSymbolKind::Constant);

    // The parameter port is reported once, not twice.
    CHECK(std::count_if(children.begin(), children.end(),
                        [](const lsDocumentSymbol& s) { return s.name == "W"; }) == 1);
}

// A labelled generate block is a named scope: what it declares belongs under it
// in the outline, not flattened into the module beside everything else.
TEST_CASE("documentSymbol: a named generate block nests what it declares",
          "[documentSymbol]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/docsym_generate.sv";
    analyzer.open(uri, "module top;\n"
                       "    logic outside;\n"
                       "    generate\n"
                       "        for (genvar i = 0; i < 4; i++) begin : g_lane\n"
                       "            sub u_sub ();\n"
                       "        end\n"
                       "    endgenerate\n"
                       "endmodule\n");

    auto result = provide_document_symbols(analyzer, make_params(uri));
    const auto* top = find_child(result, "top");
    REQUIRE(top != nullptr);

    const auto children = children_of(*top);
    CHECK(find_child(children, "outside") != nullptr);
    // Not flattened into the module.
    CHECK(find_child(children, "u_sub") == nullptr);

    const auto* block = find_child(children, "g_lane");
    REQUIRE(block != nullptr);
    CHECK(block->kind == lsSymbolKind::Namespace);

    const auto block_children = children_of(*block);
    CHECK(find_child(block_children, "u_sub") != nullptr);
}

// LSP splits the two ranges: `range` is the whole declaration, which is what
// clients fold and show in breadcrumbs; `selectionRange` is the name they
// reveal.  Reporting the name span as both collapses the outline to points.
TEST_CASE("documentSymbol: a declaration's range spans its whole body",
          "[documentSymbol]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/docsym_range.sv";
    analyzer.open(uri, "module top;\n"
                       "    logic a;\n"
                       "endmodule\n"
                       "\n"
                       "class driver;\n"
                       "    int depth;\n"
                       "endclass\n");

    auto result = provide_document_symbols(analyzer, make_params(uri));

    const auto* top = find_child(result, "top");
    REQUIRE(top != nullptr);
    CHECK(top->selectionRange.start.line == 0);
    CHECK(top->selectionRange.end.line == 0);
    CHECK(top->range.start.line == 0);
    CHECK(top->range.end.line == 2); // through `endmodule`

    const auto* cls = find_child(result, "driver");
    REQUIRE(cls != nullptr);
    CHECK(cls->selectionRange.start.line == 4);
    CHECK(cls->range.end.line == 6); // through `endclass`
}

// The outline dropped everything declared inside a labelled block, showed no
// named statement block at all, never nested a class declared inside another
// class, and collapsed every subroutine to the single line its name sits on.
TEST_CASE("document symbols: labelled blocks, nested classes and subroutine ranges",
          "[document_symbols]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/lazyverilog_docsym_blocks.sv";
    analyzer.open(uri, "module top;\n"
                       "    logic [7:0] count;\n"
                       "    always_comb begin : proc_shadow\n"
                       "        logic [7:0] shadowed;\n"
                       "        shadowed = 8'hFF;\n"
                       "    end\n"
                       "    for (genvar i = 0; i < 2; i++) begin : gen_lane\n"
                       "        logic [7:0] lane_count;\n"
                       "    end\n"
                       "    function automatic logic [7:0] f(input logic [7:0] a);\n"
                       "        return a;\n"
                       "    endfunction\n"
                       "endmodule\n"
                       "class holder;\n"
                       "    class inner_cfg;\n"
                       "        int depth;\n"
                       "    endclass\n"
                       "endclass\n");

    lsDocumentSymbolParams params;
    params.textDocument.uri.raw_uri_ = uri;
    const auto symbols = provide_document_symbols(analyzer, params);

    const std::function<const lsDocumentSymbol*(const std::vector<lsDocumentSymbol>&,
                                                const std::string&)>
        find = [&](const std::vector<lsDocumentSymbol>& nodes,
                   const std::string& name) -> const lsDocumentSymbol* {
        for (const auto& node : nodes) {
            if (node.name == name)
                return &node;
            if (node.children)
                if (const auto* hit = find(*node.children, name))
                    return hit;
        }
        return nullptr;
    };

    SECTION("a named statement block is a scope in the outline") {
        const auto* block = find(symbols, "proc_shadow");
        REQUIRE(block != nullptr);
        REQUIRE(block->children.has_value());
        CHECK(find(*block->children, "shadowed") != nullptr);
    }

    SECTION("a generate block reports its declarations") {
        const auto* block = find(symbols, "gen_lane");
        REQUIRE(block != nullptr);
        REQUIRE(block->children.has_value());
        CHECK(find(*block->children, "lane_count") != nullptr);
    }

    SECTION("a subroutine reports its whole body as its range") {
        const auto* fn = find(symbols, "f");
        REQUIRE(fn != nullptr);
        CHECK(fn->range.end.line > fn->range.start.line);
    }

    SECTION("a nested class is nested in the outline") {
        const auto* holder = find(symbols, "holder");
        REQUIRE(holder != nullptr);
        REQUIRE(holder->children.has_value());
        CHECK(find(*holder->children, "inner_cfg") != nullptr);
    }
}

// A declaration generated by a macro defined in the same file was reported at
// the `define` line, so two expansions of the same macro landed on one another
// and neither revealed anything when clicked.  A macro from an included header
// already reported the invocation site; the two now agree.
TEST_CASE("document symbols: macro-generated declarations report the invocation site",
          "[document_symbols]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/lazyverilog_docsym_macro_site.sv";
    analyzer.open(uri, "`define LANE_SIG(n) lane_``n``_valid\n"
                       "module top;\n"
                       "    logic `LANE_SIG(0);\n"
                       "    logic `LANE_SIG(1);\n"
                       "endmodule\n");

    lsDocumentSymbolParams params;
    params.textDocument.uri.raw_uri_ = uri;
    const auto symbols = provide_document_symbols(analyzer, params);

    REQUIRE(symbols.size() == 1);
    REQUIRE(symbols[0].children.has_value());
    const auto line_of = [&](const std::string& name) -> int {
        for (const auto& child : *symbols[0].children)
            if (child.name == name)
                return (int)child.selectionRange.start.line;
        return -1;
    };
    CHECK(line_of("lane_0_valid") == 2);
    CHECK(line_of("lane_1_valid") == 3);
}
