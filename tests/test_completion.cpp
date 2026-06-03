#include "analyzer.hpp"
#include "features/completion.hpp"
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>

// ── Helpers ───────────────────────────────────────────────────────────────────

static bool has_label(const CompletionList& list, const std::string& lbl) {
    return std::any_of(list.items.begin(), list.items.end(),
                       [&](const lsCompletionItem& it) { return it.label == lbl; });
}

static const lsCompletionItem* find_item(const CompletionList& list, const std::string& lbl) {
    for (const auto& it : list.items)
        if (it.label == lbl) return &it;
    return nullptr;
}

static int index_of_label(const CompletionList& list, const std::string& lbl) {
    for (int i = 0; i < (int)list.items.size(); ++i)
        if (list.items[i].label == lbl) return i;
    return -1;
}

// Return 0-based (line, col) of the first occurrence of needle in text.
static std::pair<int, int> pos_of(const std::string& text, std::string_view needle) {
    const auto off = text.find(needle);
    if (off == std::string::npos) return {0, 0};
    int line = 0, col = 0;
    for (size_t i = 0; i < off; ++i) {
        if (text[i] == '\n') { ++line; col = 0; }
        else ++col;
    }
    return {line, col};
}

static CompletionList complete_at(CompletionEngine& engine, Analyzer& analyzer,
                                   const std::string& uri, int line, int col) {
    lsTextDocumentPositionParams params;
    params.textDocument.uri.raw_uri_ = uri;
    params.position = lsPosition(line, col);
    CancellationToken tok;
    auto state = analyzer.get_state(uri);
    REQUIRE(state != nullptr);
    return engine.complete(params, *state, analyzer, tok);
}

// ── Tests ─────────────────────────────────────────────────────────────────────

TEST_CASE("completion: keywords present in module-item context", "[completion]") {
    CompletionEngine engine;
    Analyzer analyzer;
    const std::string uri = "file:///tmp/completion_kw.sv";
    analyzer.open(uri, "module top;\nendmodule\n");

    auto result = complete_at(engine, analyzer, uri, 1, 0);
    CHECK(has_label(result, "always_ff"));
    CHECK(has_label(result, "assign"));
    CHECK_FALSE(has_label(result, "break"));
}

TEST_CASE("completion: keywords present in general identifier context", "[completion]") {
    CompletionEngine engine;
    Analyzer analyzer;
    const std::string uri = "file:///tmp/completion_kw_general.sv";
    analyzer.open(uri, "");

    auto result = complete_at(engine, analyzer, uri, 0, 0);
    CHECK(has_label(result, "module"));
    CHECK(has_label(result, "interface"));
    CHECK_FALSE(has_label(result, "assign"));
}

TEST_CASE("completion: keywords are context-aware", "[completion]") {
    CompletionEngine engine;
    Analyzer analyzer;
    const std::string uri = "file:///tmp/completion_kw_context.sv";
    const std::string text =
        "module top;\n"
        "    always_comb begin\n"
        "        \n"
        "    end\n"
        "    class packet;\n"
        "        \n"
        "    endclass\n"
        "    covergroup cg;\n"
        "        \n"
        "    endgroup\n"
        "endmodule\n";
    analyzer.open(uri, text);

    auto procedural = complete_at(engine, analyzer, uri, 2, 8);
    CHECK(has_label(procedural, "if"));
    CHECK(has_label(procedural, "case"));
    CHECK_FALSE(has_label(procedural, "endmodule"));

    auto cls = complete_at(engine, analyzer, uri, 5, 8);
    CHECK(has_label(cls, "function"));
    CHECK(has_label(cls, "constraint"));
    CHECK_FALSE(has_label(cls, "assign"));

    auto covergroup = complete_at(engine, analyzer, uri, 8, 8);
    CHECK(has_label(covergroup, "coverpoint"));
    CHECK(has_label(covergroup, "bins"));
    CHECK_FALSE(has_label(covergroup, "assign"));
}

TEST_CASE("completion: module defined in same file appears as identifier", "[completion]") {
    CompletionEngine engine;
    Analyzer analyzer;
    const std::string uri = "file:///tmp/completion_ident.sv";
    // The module m_alu is defined; cursor placed at identifier position in top
    const std::string text =
        "module m_alu(input logic i_a, output logic o_r);\nendmodule\n"
        "module top;\n"
        "    m\n"   // typing 'm' at col 4
        "endmodule\n";
    analyzer.open(uri, text);

    auto [line, col] = pos_of(text, "\n    m\n");
    // Advance to the 'm' character
    auto result = complete_at(engine, analyzer, uri, line + 1, 5);
    CHECK(has_label(result, "m_alu"));
}

TEST_CASE("completion: module-local variables appear as identifiers", "[completion]") {
    CompletionEngine engine;
    Analyzer analyzer;
    const std::string uri = "file:///tmp/completion_local_vars.sv";
    const std::string text =
        "module top;\n"
        "    logic [7:0] data23;\n"
        "    logic [7:0] data32;\n"
        "    assign data32 = \n"
        "endmodule\n";
    analyzer.open(uri, text);

    auto result = complete_at(engine, analyzer, uri, 3, 20);

    CHECK(has_label(result, "data23"));
    CHECK(has_label(result, "data32"));
}

TEST_CASE("completion: enum assignment prioritizes matching enum literals", "[completion]") {
    CompletionEngine engine;
    Analyzer analyzer;
    const std::string uri = "file:///tmp/completion_enum_rhs.sv";
    const std::string text =
        "typedef enum logic [1:0] {\n"
        "    IDLE,\n"
        "    TRAIN,\n"
        "    DONE\n"
        "} state_t;\n"
        "module top;\n"
        "    state_t state;\n"
        "    logic unrelated;\n"
        "    always_comb begin\n"
        "        state = \n"
        "    end\n"
        "endmodule\n";
    analyzer.open(uri, text);

    auto result = complete_at(engine, analyzer, uri, 9, 16);

    REQUIRE(has_label(result, "IDLE"));
    REQUIRE(has_label(result, "TRAIN"));
    REQUIRE(has_label(result, "DONE"));
    REQUIRE(has_label(result, "unrelated"));

    CHECK(index_of_label(result, "IDLE") < index_of_label(result, "unrelated"));
    CHECK(index_of_label(result, "TRAIN") < index_of_label(result, "unrelated"));
    CHECK(index_of_label(result, "DONE") < index_of_label(result, "unrelated"));
}

TEST_CASE("completion: assignment prioritizes same-type signals", "[completion]") {
    CompletionEngine engine;
    Analyzer analyzer;
    const std::string uri = "file:///tmp/completion_same_type_rhs.sv";
    const std::string text =
        "typedef enum logic [1:0] { IDLE, RUN, DONE } state_t;\n"
        "module top;\n"
        "    state_t state, r_state;\n"
        "    logic unrelated;\n"
        "    always_comb begin\n"
        "        state =\n"
        "    end\n"
        "endmodule\n";
    analyzer.open(uri, text);

    auto result = complete_at(engine, analyzer, uri, 5, 15);

    REQUIRE(has_label(result, "r_state"));
    REQUIRE(has_label(result, "unrelated"));
    CHECK(index_of_label(result, "r_state") < index_of_label(result, "unrelated"));
}

TEST_CASE("completion: enum nonblocking assignment prioritizes matching enum literals", "[completion]") {
    CompletionEngine engine;
    Analyzer analyzer;
    const std::string uri = "file:///tmp/completion_enum_nonblocking_rhs.sv";
    const std::string text =
        "typedef enum logic [1:0] { S_IDLE, S_BUSY, S_DONE } state_t;\n"
        "module top;\n"
        "    state_t state;\n"
        "    logic unrelated;\n"
        "    always_ff @(posedge clk) begin\n"
        "        state <= \n"
        "    end\n"
        "endmodule\n";
    analyzer.open(uri, text);

    auto result = complete_at(engine, analyzer, uri, 5, 17);

    REQUIRE(has_label(result, "S_IDLE"));
    REQUIRE(has_label(result, "S_BUSY"));
    REQUIRE(has_label(result, "S_DONE"));
    REQUIRE(has_label(result, "unrelated"));
    CHECK(index_of_label(result, "S_IDLE") < index_of_label(result, "unrelated"));
    CHECK(index_of_label(result, "S_BUSY") < index_of_label(result, "unrelated"));
    CHECK(index_of_label(result, "S_DONE") < index_of_label(result, "unrelated"));
}

TEST_CASE("completion: nonblocking assignment prioritizes same-type signals", "[completion]") {
    CompletionEngine engine;
    Analyzer analyzer;
    const std::string uri = "file:///tmp/completion_same_type_nonblocking_rhs.sv";
    const std::string text =
        "typedef enum logic [1:0] { IDLE, RUN, DONE } state_t;\n"
        "module top;\n"
        "    state_t state, r_state;\n"
        "    int unrelated;\n"
        "    always_ff @(posedge clk) begin\n"
        "        state <=\n"
        "    end\n"
        "endmodule\n";
    analyzer.open(uri, text);

    auto result = complete_at(engine, analyzer, uri, 5, 16);

    REQUIRE(has_label(result, "r_state"));
    REQUIRE(has_label(result, "unrelated"));
    CHECK(index_of_label(result, "r_state") < index_of_label(result, "unrelated"));
}

TEST_CASE("completion: enum assignment respects typed prefix", "[completion]") {
    CompletionEngine engine;
    Analyzer analyzer;
    const std::string uri = "file:///tmp/completion_enum_rhs_prefix.sv";
    const std::string text =
        "typedef enum logic [1:0] { IDLE, TRAIN, DONE } state_t;\n"
        "module top;\n"
        "    state_t state;\n"
        "    always_comb begin\n"
        "        state = TR\n"
        "    end\n"
        "endmodule\n";
    analyzer.open(uri, text);

    auto result = complete_at(engine, analyzer, uri, 4, 18);

    CHECK(has_label(result, "TRAIN"));
    CHECK(!has_label(result, "IDLE"));
    CHECK(!has_label(result, "DONE"));
}

TEST_CASE("completion: NamedPort context suggests unconnected ports", "[completion]") {
    CompletionEngine engine;
    Analyzer analyzer;
    const std::string uri = "file:///tmp/completion_namedport.sv";
    const std::string text =
        "module m_fifo(\n"
        "    input  logic i_clk,\n"
        "    input  logic i_data,\n"
        "    output logic o_data\n"
        ");\n"
        "endmodule\n"
        "module top;\n"
        "    m_fifo u0 (.\n"  // cursor after '.' on this line
        ");\n"
        "endmodule\n";
    analyzer.open(uri, text);

    // Line 7: "    m_fifo u0 (."  → col 16 is after the dot
    auto result = complete_at(engine, analyzer, uri, 7, 16);

    CHECK(has_label(result, ".i_clk"));
    CHECK(has_label(result, ".i_data"));
    CHECK(has_label(result, ".o_data"));

    // Should NOT see module-level keywords mixed in as named ports
    CHECK(!has_label(result, "module"));
}

TEST_CASE("completion: NamedPort filters already-connected ports", "[completion]") {
    CompletionEngine engine;
    Analyzer analyzer;
    const std::string uri = "file:///tmp/completion_namedport_filter.sv";
    const std::string text =
        "module m_fifo(\n"
        "    input  logic i_clk,\n"
        "    input  logic i_data,\n"
        "    output logic o_data\n"
        ");\n"
        "endmodule\n"
        "module top;\n"
        "    m_fifo u0 (.i_clk(clk), .\n"  // i_clk already connected
        ");\n"
        "endmodule\n";
    analyzer.open(uri, text);

    // Col after the trailing dot on line 7
    const std::string needle = ".i_clk(clk), .";
    auto [line, col] = pos_of(text, needle);
    // col points to start of needle; advance to after the trailing '.'
    auto result = complete_at(engine, analyzer, uri, line, col + (int)needle.size());

    CHECK(!has_label(result, ".i_clk")); // already connected — must be absent
    CHECK(has_label(result, ".i_data"));
    CHECK(has_label(result, ".o_data"));
}

TEST_CASE("completion: NamedPort works after previous last connection line", "[completion]") {
    CompletionEngine engine;
    Analyzer analyzer;
    const std::string uri = "file:///tmp/completion_after_last_connection.sv";

    // This mirrors editing below the final connection in demo/memory_top.sv:
    // the previous line has a complete ".o_data(...)" connection and no
    // trailing comma yet.  Completion context detection should still climb out
    // of the connection expression parentheses, find the enclosing instance
    // port list, and offer any *remaining* unconnected ports.
    const std::string text =
        "module m_dut(\n"
        "    input  logic i_clk,\n"
        "    input  logic address,\n"
        "    input  logic i_data,\n"
        "    output logic o_data,\n"
        "    output logic o_extra\n"
        ");\n"
        "endmodule\n"
        "module top;\n"
        "    m_dut u0 (\n"
        "        .i_clk   (clk),\n"
        "        .address (addr),\n"
        "        .i_data  (data_i),\n"
        "        .o_data  (data_o)\n"
        "        .\n"
        "    );\n"
        "endmodule\n";
    analyzer.open(uri, text);

    auto result = complete_at(engine, analyzer, uri, 14, 9);

    CHECK(has_label(result, ".o_extra"));
    CHECK(!has_label(result, ".i_clk"));
    CHECK(!has_label(result, ".address"));
    CHECK(!has_label(result, ".i_data"));
    CHECK(!has_label(result, ".o_data"));
}

TEST_CASE("completion: NamedPort returns empty when all ports are connected", "[completion]") {
    CompletionEngine engine;
    Analyzer analyzer;
    const std::string uri = "file:///tmp/completion_after_all_connected.sv";

    // If every real port is already connected, named-port completion should be
    // intentionally empty.  This keeps auto-instantiation suggestions focused
    // on missing work instead of offering duplicate connections.
    const std::string text =
        "module memory(\n"
        "    input  logic i_clk,\n"
        "    input  logic address,\n"
        "    input  logic i_data,\n"
        "    output logic o_data\n"
        ");\n"
        "endmodule\n"
        "module top;\n"
        "    memory u_mem3 (\n"
        "        .i_clk   (clk),\n"
        "        .address (addr),\n"
        "        .i_data  (data23),\n"
        "        .o_data  (data32)\n"
        "        .\n"
        "    );\n"
        "endmodule\n";
    analyzer.open(uri, text);

    auto result = complete_at(engine, analyzer, uri, 13, 9);

    CHECK(result.items.empty());
}

TEST_CASE("completion: NamedPort snippet format", "[completion]") {
    CompletionEngine engine;
    Analyzer analyzer;
    const std::string uri = "file:///tmp/completion_namedport_snippet.sv";
    const std::string text =
        "module m_dut(input logic i_clk);\nendmodule\n"
        "module top;\n"
        "    m_dut u0 (.\n"
        ");\n"
        "endmodule\n";
    analyzer.open(uri, text);

    auto result = complete_at(engine, analyzer, uri, 3, 15);
    const auto* item = find_item(result, ".i_clk");
    REQUIRE(item != nullptr);
    REQUIRE(item->insertText.has_value());
    CHECK(*item->insertText == "i_clk(${1:i_clk})");
    REQUIRE(item->insertTextFormat.has_value());
    CHECK(*item->insertTextFormat == lsInsertTextFormat::Snippet);
}

TEST_CASE("completion: NamedPort snippet uses port name placeholder", "[completion]") {
    CompletionEngine engine;
    Analyzer analyzer;
    const std::string uri = "file:///tmp/completion_namedport_signal_match.sv";
    const std::string text =
        "module m_dut(input logic i_clk, output logic o_data);\nendmodule\n"
        "module top;\n"
        "    logic clk;\n"
        "    logic data;\n"
        "    m_dut u0 (.\n"
        ");\n"
        "endmodule\n";
    analyzer.open(uri, text);

    auto result = complete_at(engine, analyzer, uri, 5, 15);
    const auto* item = find_item(result, ".i_clk");
    REQUIRE(item != nullptr);
    REQUIRE(item->insertText.has_value());
    CHECK(*item->insertText == "i_clk(${1:i_clk})");
}

TEST_CASE("completion: NamedPort excludes module parameters", "[completion]") {
    CompletionEngine engine;
    Analyzer analyzer;
    const std::string uri = "file:///tmp/completion_namedport_no_params.sv";

    // Assumption/regression target:
    //   A bare '.' in the *instance port list* must suggest only ports, even
    //   when the instantiated module has a parameter list.  The SyntaxIndex
    //   stores parameter declarations alongside ports for older features, so
    //   the completion provider must explicitly filter them.
    const std::string text =
        "module m_dut #(\n"
        "    parameter int WIDTH = 8,\n"
        "    localparam int DEPTH = 16\n"
        ") (\n"
        "    input  logic i_clk,\n"
        "    output logic o_data\n"
        ");\n"
        "endmodule\n"
        "module top;\n"
        "    m_dut u0 (.\n"
        ");\n"
        "endmodule\n";
    analyzer.open(uri, text);

    auto result = complete_at(engine, analyzer, uri, 9, 15);

    CHECK(has_label(result, ".i_clk"));
    CHECK(has_label(result, ".o_data"));
    CHECK(!has_label(result, ".WIDTH"));
    CHECK(!has_label(result, ".DEPTH"));
}

TEST_CASE("completion: NamedPort insertText omits typed dot", "[completion]") {
    CompletionEngine engine;
    Analyzer analyzer;
    const std::string uri = "file:///tmp/completion_namedport_textedit.sv";
    const std::string text =
        "module m_dut(input logic i_clk);\n"
        "endmodule\n"
        "module top;\n"
        "    m_dut u0 (.\n"
        ");\n"
        "endmodule\n";
    analyzer.open(uri, text);

    auto result = complete_at(engine, analyzer, uri, 3, 15);
    const auto* item = find_item(result, ".i_clk");
    REQUIRE(item != nullptr);

    // The editor buffer already contains the trigger '.', so insertText must
    // not include another one.  Relying on ordinary word replacement instead
    // of snippet textEdit avoids duplicate dots; the Neovim plugin disables
    // completion popup/preview for lazyverilog buffers so snippet placeholders
    // are only expanded on accept.
    REQUIRE(item->insertText.has_value());
    CHECK(*item->insertText == "i_clk(${1:i_clk})");
    REQUIRE(item->insertTextFormat.has_value());
    CHECK(*item->insertTextFormat == lsInsertTextFormat::Snippet);
    CHECK(!item->textEdit.has_value());
}

TEST_CASE("completion: Parameter context suggests params in #()", "[completion]") {
    CompletionEngine engine;
    Analyzer analyzer;
    const std::string uri = "file:///tmp/completion_param.sv";
    const std::string text =
        "module m_mem #(\n"
        "    parameter int W_DEPTH = 256,\n"
        "    parameter int W_WIDTH = 8\n"
        ") (\n"
        "    input logic i_clk\n"
        ");\n"
        "endmodule\n"
        "module top;\n"
        "    m_mem #(.\n"  // cursor after dot inside #()
        ") u0 ();\n"
        "endmodule\n";
    analyzer.open(uri, text);

    // Line 8: "    m_mem #(."  → col 13 is after the dot
    auto result = complete_at(engine, analyzer, uri, 8, 13);

    CHECK(has_label(result, ".W_DEPTH"));
    CHECK(has_label(result, ".W_WIDTH"));
    CHECK(!has_label(result, ".i_clk"));
}

TEST_CASE("completion: Parameter insertText omits typed dot", "[completion]") {
    CompletionEngine engine;
    Analyzer analyzer;
    const std::string uri = "file:///tmp/completion_param_textedit.sv";
    const std::string text =
        "module m_dut #(parameter int WIDTH = 8) (input logic i_clk);\n"
        "endmodule\n"
        "module top;\n"
        "    m_dut #(.WI\n"
        ") u0 ();\n"
        "endmodule\n";
    analyzer.open(uri, text);

    // Cursor is after the partially typed ".WI".  Completion clients replace
    // the word prefix ("WI") and leave the already-typed trigger dot in place.
    auto result = complete_at(engine, analyzer, uri, 3, 15);
    const auto* item = find_item(result, ".WIDTH");
    REQUIRE(item != nullptr);
    REQUIRE(item->insertText.has_value());
    CHECK(*item->insertText == "WIDTH(${1:})");
    REQUIRE(item->insertTextFormat.has_value());
    CHECK(*item->insertTextFormat == lsInsertTextFormat::Snippet);
    CHECK(!item->textEdit.has_value());
}

TEST_CASE("completion: Parameter shows default value in detail", "[completion]") {
    CompletionEngine engine;
    Analyzer analyzer;
    const std::string uri = "file:///tmp/completion_param_detail.sv";
    const std::string text =
        "module m_buf #(parameter int W_DATA = 32) (input logic i_clk);\n"
        "endmodule\n"
        "module top;\n"
        "    m_buf #(.\n"
        ") u0 ();\n"
        "endmodule\n";
    analyzer.open(uri, text);

    auto result = complete_at(engine, analyzer, uri, 3, 13);
    const auto* item = find_item(result, ".W_DATA");
    REQUIRE(item != nullptr);
    REQUIRE(item->detail.has_value());
    CHECK(item->detail->find("32") != std::string::npos);
}

TEST_CASE("completion: Macro context returns macro names", "[completion]") {
    CompletionEngine engine;
    Analyzer analyzer;
    const std::string uri = "file:///tmp/completion_macro.sv";
    const std::string text =
        "`define MY_WIDTH 32\n"
        "`define MY_FUNC(a, b) ((a) + (b))\n"
        "module top;\n"
        "    logic [`\n"  // cursor after backtick → Macro context
        "endmodule\n";
    analyzer.open(uri, text);

    // Line 3: "    logic [`"  → cursor at col 12 (after backtick)
    auto result = complete_at(engine, analyzer, uri, 3, 12);

    CHECK(has_label(result, "MY_WIDTH"));
    CHECK(has_label(result, "MY_FUNC"));
    // Should not suggest keywords in this context
    CHECK(!has_label(result, "module"));
}

TEST_CASE("completion: Macro function-like gets snippet with params", "[completion]") {
    CompletionEngine engine;
    Analyzer analyzer;
    const std::string uri = "file:///tmp/completion_macro_snip.sv";
    const std::string text =
        "`define CLAMP(val, lo, hi) ((val) < (lo) ? (lo) : (val) > (hi) ? (hi) : (val))\n"
        "module top;\n"
        "    logic x = `\n"
        "endmodule\n";
    analyzer.open(uri, text);

    auto result = complete_at(engine, analyzer, uri, 2, 15);
    const auto* item = find_item(result, "CLAMP");
    REQUIRE(item != nullptr);
    REQUIRE(item->insertText.has_value());
    // Snippet should contain all three param placeholders
    CHECK(item->insertText->find("val") != std::string::npos);
    CHECK(item->insertText->find("lo") != std::string::npos);
    CHECK(item->insertText->find("hi") != std::string::npos);
}

TEST_CASE("completion: PackageScope context returns package symbols", "[completion]") {
    CompletionEngine engine;
    Analyzer analyzer;
    const std::string uri = "file:///tmp/completion_pkgscope.sv";
    const std::string text =
        "package my_pkg;\n"
        "    typedef logic [7:0] byte_t;\n"
        "    parameter int TIMEOUT = 100;\n"
        "endpackage\n"
        "module top;\n"
        "    my_pkg::\n"  // cursor after '::'
        "endmodule\n";
    analyzer.open(uri, text);

    // Line 5: "    my_pkg::"  → col 12 after '::'
    auto result = complete_at(engine, analyzer, uri, 5, 12);

    CHECK(has_label(result, "byte_t"));
    CHECK(has_label(result, "TIMEOUT"));
}

TEST_CASE("completion: PackageScope context returns package classes", "[completion]") {
    CompletionEngine engine;
    Analyzer analyzer;
    const std::string uri = "file:///tmp/completion_pkgscope_class.sv";
    const std::string text =
        "package uvm_pkg;\n"
        "    class uvm_object;\n"
        "    endclass\n"
        "    class uvm_component;\n"
        "    endclass\n"
        "endpackage\n"
        "module top;\n"
        "    uvm_pkg::\n"
        "endmodule\n";
    analyzer.open(uri, text);

    auto [line, col] = pos_of(text, "uvm_pkg::");
    auto result = complete_at(engine, analyzer, uri, line, col + (int)std::string("uvm_pkg::").size());

    CHECK(has_label(result, "uvm_object"));
    CHECK(has_label(result, "uvm_component"));
}

TEST_CASE("completion: package scope works inside procedural block", "[completion]") {
    CompletionEngine engine;
    Analyzer analyzer;
    const std::string uri = "file:///tmp/completion_pkgscope_procedural.sv";
    const std::string text =
        "package uvm_pkg;\n"
        "    class uvm_object;\n"
        "    endclass\n"
        "endpackage\n"
        "module top;\n"
        "    initial begin\n"
        "        uvm_pkg::\n"
        "    end\n"
        "endmodule\n";
    analyzer.open(uri, text);

    auto [line, col] = pos_of(text, "uvm_pkg::");
    auto result = complete_at(engine, analyzer, uri, line, col + (int)std::string("uvm_pkg::").size());

    CHECK(has_label(result, "uvm_object"));
}

TEST_CASE("completion: demo UVM package scope returns indexed UVM symbols", "[completion]") {
    CompletionEngine engine;
    Analyzer analyzer;

    auto root = std::filesystem::current_path();
    if (!std::filesystem::exists(root / "demo") && std::filesystem::exists(root.parent_path() / "demo"))
        root = root.parent_path();
    const auto demo_file = root / "demo" / "uvm_completion_demo.sv";
    const auto vcode = root / "demo" / "vcode.f";
    REQUIRE(std::filesystem::exists(demo_file));
    REQUIRE(std::filesystem::exists(vcode));

    // Mirror the real demo setup: UVM is intentionally treated like any other
    // source library.  vcode.f lists uvm_pkg.sv as the explicit source and
    // +incdir+ points slang at the headers included by that package.
    std::vector<std::string> files;
    std::vector<std::string> include_dirs;
    std::ifstream filelist(vcode);
    REQUIRE(filelist.good());
    std::string filelist_line;
    while (std::getline(filelist, filelist_line)) {
        if (auto pos = filelist_line.find("//"); pos != std::string::npos)
            filelist_line.erase(pos);
        if (auto pos = filelist_line.find('#'); pos != std::string::npos)
            filelist_line.erase(pos);
        auto first = filelist_line.find_first_not_of(" \t\r\n");
        if (first == std::string::npos)
            continue;
        auto last = filelist_line.find_last_not_of(" \t\r\n");
        auto item = filelist_line.substr(first, last - first + 1);
        if (item.starts_with("+incdir+")) {
            std::string rest = item.substr(std::string("+incdir+").size());
            size_t start = 0;
            while (start <= rest.size()) {
                const size_t plus = rest.find('+', start);
                auto dir_text = rest.substr(start, plus == std::string::npos
                                                       ? std::string::npos
                                                       : plus - start);
                if (!dir_text.empty()) {
                    auto dir = std::filesystem::path(dir_text);
                    if (dir.is_relative())
                        dir = vcode.parent_path() / dir;
                    include_dirs.push_back(std::filesystem::absolute(dir).lexically_normal().string());
                }
                if (plus == std::string::npos)
                    break;
                start = plus + 1;
            }
            continue;
        }
        if (item.starts_with("+") || item.starts_with("-"))
            continue;
        auto path = std::filesystem::path(item);
        if (path.is_relative())
            path = vcode.parent_path() / path;
        files.push_back(std::filesystem::absolute(path).lexically_normal().string());
    }
    REQUIRE(!files.empty());
    REQUIRE(!include_dirs.empty());
    analyzer.set_include_dirs(include_dirs);
    analyzer.set_extra_files(files);

    std::ifstream input(demo_file);
    REQUIRE(input.good());
    const std::string text((std::istreambuf_iterator<char>(input)),
                           std::istreambuf_iterator<char>());
    const std::string uri = "file://" + demo_file.string();
    analyzer.open(uri, text);

    auto [line, col] = pos_of(text, "uvm_pkg::");
    auto result = complete_at(engine, analyzer, uri, line,
                              col + (int)std::string("uvm_pkg::").size());

    CHECK(has_label(result, "uvm_object"));
    CHECK(has_label(result, "uvm_component"));
}

TEST_CASE("completion: parameterized class scope returns static methods", "[completion]") {
    CompletionEngine engine;
    Analyzer analyzer;
    const std::string uri = "file:///tmp/completion_param_class_scope.sv";
    const std::string text =
        "class uvm_object;\n"
        "endclass\n"
        "class uvm_config_db #(type T = int);\n"
        "    static function bit get(string inst_name);\n"
        "    endfunction\n"
        "    static function void set(string inst_name, T value);\n"
        "    endfunction\n"
        "endclass\n"
        "module top;\n"
        "    initial begin\n"
        "        uvm_config_db#(uvm_object)::\n"
        "    end\n"
        "endmodule\n";
    analyzer.open(uri, text);

    auto [line, col] = pos_of(text, "uvm_config_db#(uvm_object)::");
    auto result = complete_at(engine, analyzer, uri, line, col + (int)std::string("uvm_config_db#(uvm_object)::").size());

    CHECK(has_label(result, "get"));
    CHECK(has_label(result, "set"));
}

TEST_CASE("completion: MemberAccess returns class fields by type name", "[completion]") {
    CompletionEngine engine;
    Analyzer analyzer;
    const std::string uri = "file:///tmp/completion_member.sv";
    // MemberProvider resolves by the name to the left of '.'.
    // Use the class name itself as the LHS (best-effort, works for type names).
    const std::string text =
        "class pkt;\n"
        "    logic [7:0] addr;\n"
        "    int         data;\n"
        "endclass\n"
        "module top;\n"
        "    pkt.\n"  // 'pkt' is the class type name — MemberProvider looks it up
        "endmodule\n";
    analyzer.open(uri, text);

    // Line 5: "    pkt."  → col 8 (after the dot)
    auto result = complete_at(engine, analyzer, uri, 5, 8);

    CHECK(has_label(result, "addr"));
    CHECK(has_label(result, "data"));
}

TEST_CASE("completion: MemberAccess resolves class variable type", "[completion]") {
    CompletionEngine engine;
    Analyzer analyzer;
    const std::string uri = "file:///tmp/completion_member_var_type.sv";
    const std::string text =
        "class pkt_cfg;\n"
        "    int depth;\n"
        "    function void apply();\n"
        "    endfunction\n"
        "endclass\n"
        "module top;\n"
        "    pkt_cfg cfg;\n"
        "    initial cfg.\n"
        "endmodule\n";
    analyzer.open(uri, text);

    auto result = complete_at(engine, analyzer, uri, 7, 16);

    CHECK(has_label(result, "depth"));
    CHECK(has_label(result, "apply"));
}

TEST_CASE("completion: MemberAccess resolves typedef struct variable type", "[completion]") {
    CompletionEngine engine;
    Analyzer analyzer;
    const std::string uri = "file:///tmp/completion_member_struct_var_type.sv";
    const std::string text =
        "typedef struct packed {\n"
        "    logic [7:0] addr;\n"
        "    logic       valid;\n"
        "} packet_t;\n"
        "module top;\n"
        "    packet_t pkt;\n"
        "    initial pkt.\n"
        "endmodule\n";
    analyzer.open(uri, text);

    auto result = complete_at(engine, analyzer, uri, 6, 16);

    CHECK(has_label(result, "addr"));
    CHECK(has_label(result, "valid"));
}

TEST_CASE("completion: EventControl suggests local signals", "[completion]") {
    CompletionEngine engine;
    Analyzer analyzer;
    const std::string uri = "file:///tmp/completion_event_control.sv";
    const std::string text =
        "module top;\n"
        "    logic clk;\n"
        "    logic rst_n;\n"
        "    always_ff @(posedge \n"
        "endmodule\n";
    analyzer.open(uri, text);

    auto result = complete_at(engine, analyzer, uri, 3, 24);

    CHECK(has_label(result, "clk"));
    CHECK(has_label(result, "rst_n"));
}

TEST_CASE("completion: NewExpression suggests classes", "[completion]") {
    CompletionEngine engine;
    Analyzer analyzer;
    const std::string uri = "file:///tmp/completion_new_expr.sv";
    const std::string text =
        "class pkt_cfg;\n"
        "endclass\n"
        "module top;\n"
        "    initial begin\n"
        "        automatic pkt_cfg cfg = new \n"
        "    end\n"
        "endmodule\n";
    analyzer.open(uri, text);

    auto result = complete_at(engine, analyzer, uri, 4, 36);

    CHECK(has_label(result, "pkt_cfg"));
}

TEST_CASE("completion: snippet items included in identifier context", "[completion]") {
    CompletionEngine engine;
    Analyzer analyzer;
    const std::string uri = "file:///tmp/completion_snippet.sv";
    analyzer.open(uri, "module top;\n    al\nendmodule\n");

    auto result = complete_at(engine, analyzer, uri, 1, 6);

    // Structural snippets should appear
    CHECK(has_label(result, "always_ff"));
    CHECK(has_label(result, "always_comb"));
}

TEST_CASE("completion: degradation never returns empty for identifier context", "[completion]") {
    CompletionEngine engine;
    Analyzer analyzer;
    const std::string uri = "file:///tmp/completion_degrade.sv";
    // Malformed file — should still return keywords
    analyzer.open(uri, "xyzzy blorp (\n");

    auto result = complete_at(engine, analyzer, uri, 0, 0);
    CHECK(!result.items.empty());
}

TEST_CASE("completion: local module ranks above extra-file module with same prefix", "[completion]") {
    const auto extra_path =
        std::filesystem::temp_directory_path() / "completion_scope_extra.sv";
    {
        std::ofstream out(extra_path);
        out << "module m_extra_adder(input logic i_a);\nendmodule\n";
    }

    CompletionEngine engine;
    Analyzer analyzer;
    analyzer.set_extra_files({extra_path.string()});

    const std::string uri = "file:///tmp/completion_scope_local.sv";
    // m_local_adder declared in the open document
    const std::string text =
        "module m_local_adder(input logic i_a);\nendmodule\n"
        "module top;\n"
        "    m\n"
        "endmodule\n";
    analyzer.open(uri, text);

    auto result = complete_at(engine, analyzer, uri, 3, 5);

    // Both should appear
    REQUIRE(has_label(result, "m_local_adder"));
    REQUIRE(has_label(result, "m_extra_adder"));

    // Local item must sort before the extra-file item
    int local_pos = -1, extra_pos = -1;
    for (int i = 0; i < (int)result.items.size(); ++i) {
        if (result.items[i].label == "m_local_adder") local_pos = i;
        if (result.items[i].label == "m_extra_adder") extra_pos = i;
    }
    CHECK(local_pos < extra_pos);

    std::filesystem::remove(extra_path);
}

TEST_CASE("completion: FileProvider returns svh from extra files", "[completion]") {
    const auto header_path =
        std::filesystem::temp_directory_path() / "my_params.svh";
    {
        std::ofstream out(header_path);
        out << "// header\n";
    }

    CompletionEngine engine;
    Analyzer analyzer;
    analyzer.set_extra_files({header_path.string()});

    const std::string uri = "file:///tmp/completion_file.sv";
    // `include " triggers IncludeFile context
    const std::string text = "module top;\nendmodule\n`include \"\n";
    analyzer.open(uri, text);

    // Line 2: "`include \""  → col 10 (after the quote)
    auto result = complete_at(engine, analyzer, uri, 2, 10);

    CHECK(has_label(result, "my_params.svh"));
    // .sv files should NOT appear (only .svh/.vh)
    CHECK(!has_label(result, "completion_file.sv"));

    std::filesystem::remove(header_path);
}
