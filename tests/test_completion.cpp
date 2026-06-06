#include "analyzer.hpp"
#include "features/completion.hpp"
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <vector>

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

static int count_label(const CompletionList& list, const std::string& lbl) {
    return (int)std::count_if(list.items.begin(), list.items.end(),
                              [&](const lsCompletionItem& it) { return it.label == lbl; });
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

static std::pair<int, int> pos_after(const std::string& text, std::string_view needle) {
    auto [line, col] = pos_of(text, needle);
    for (const char ch : needle) {
        if (ch == '\n') {
            ++line;
            col = 0;
        } else {
            ++col;
        }
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

struct SyntheticUvmFixture {
    std::filesystem::path root;
    std::filesystem::path package_file;
    std::filesystem::path macro_header;
    std::vector<std::string> files;
    std::vector<std::string> include_dirs;
};

static void write_text_file(const std::filesystem::path& path, std::string_view text) {
    std::ofstream out(path);
    REQUIRE(out.good());
    out << text;
    REQUIRE(out.good());
}

static SyntheticUvmFixture make_synthetic_uvm_fixture(std::string_view name) {
    SyntheticUvmFixture setup;
    setup.root = std::filesystem::temp_directory_path() /
                 ("lazyverilog_completion_" + std::string(name));
    std::filesystem::create_directories(setup.root);

    setup.package_file = setup.root / "uvm_pkg.sv";
    setup.macro_header = setup.root / "uvm_macros.svh";

    // These tests intentionally use a tiny synthetic package instead of any
    // repository fixture or downloaded UVM tree.  It contains only the
    // declarations needed to exercise the same completion paths a real UVM
    // installation uses:
    //
    //   - project-indexed package members for `uvm_pkg::...`;
    //   - a parameterized class for `uvm_config_db#(...)::...`; and
    //   - an include-resolved macro header for backtick macro completion.
    //
    // Keeping the fixture local to /tmp makes the tests hermetic while still
    // proving that there is no special hard-coded UVM completion provider.
    write_text_file(setup.package_file,
                    "package uvm_pkg;\n"
                    "  class uvm_object;\n"
                    "  endclass\n"
                    "  class uvm_component extends uvm_object;\n"
                    "  endclass\n"
                    "  class uvm_sequence_item extends uvm_object;\n"
                    "  endclass\n"
                    "  class uvm_env extends uvm_component;\n"
                    "  endclass\n"
                    "  class uvm_phase;\n"
                    "  endclass\n"
                    "  class uvm_config_db #(type T = int);\n"
                    "    static function bit get();\n"
                    "      return 1'b0;\n"
                    "    endfunction\n"
                    "    static function void set();\n"
                    "    endfunction\n"
                    "    static function bit exists();\n"
                    "      return 1'b0;\n"
                    "    endfunction\n"
                    "    static task wait_modified();\n"
                    "    endtask\n"
                    "  endclass\n"
                    "endpackage\n");

    write_text_file(setup.macro_header,
                    "`define uvm_object_utils(T)\n"
                    "`define uvm_component_utils(T)\n"
                    "`define uvm_info(ID, MSG, VERBOSITY)\n"
                    "`define uvm_warning(ID, MSG)\n"
                    "`define uvm_error(ID, MSG)\n"
                    "`define uvm_fatal(ID, MSG)\n");

    setup.files.push_back(std::filesystem::absolute(setup.package_file)
                              .lexically_normal()
                              .string());
    setup.include_dirs.push_back(std::filesystem::absolute(setup.root)
                                     .lexically_normal()
                                     .string());
    return setup;
}

static void configure_synthetic_uvm_index(Analyzer& analyzer, const SyntheticUvmFixture& setup) {
    analyzer.set_include_dirs(setup.include_dirs);
    analyzer.set_extra_files(setup.files);
    analyzer.wait_for_background_index_idle();
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
    CHECK(has_label(result, "function"));
    CHECK(has_label(result, "task"));
    CHECK_FALSE(has_label(result, "assign"));
    CHECK_FALSE(has_label(result, "always_comb"));
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
    // Snippets share labels with keywords in the UI.  Class scope must not get
    // module/procedural structural snippets such as always_comb.
    CHECK_FALSE(has_label(cls, "always_comb"));
    CHECK_FALSE(has_label(cls, "always_ff"));
    CHECK_FALSE(has_label(cls, "module"));

    auto covergroup = complete_at(engine, analyzer, uri, 8, 8);
    CHECK(has_label(covergroup, "coverpoint"));
    CHECK(has_label(covergroup, "bins"));
    CHECK_FALSE(has_label(covergroup, "assign"));
    CHECK_FALSE(has_label(covergroup, "always_comb"));
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

TEST_CASE("completion: identifier visibility respects lexical scope", "[completion]") {
    CompletionEngine engine;
    Analyzer analyzer;
    const std::string uri = "file:///tmp/completion_scope_visibility.sv";
    const std::string text =
        "module other;\n"
        "    logic hidden_other;\n"
        "endmodule\n"
        "module top;\n"
        "    logic visible_top;\n"
        "    always_comb begin\n"
        "        logic visible_block;\n"
        "        \n"
        "    end\n"
        "    \n"
        "endmodule\n";
    analyzer.open(uri, text);

    auto in_block = complete_at(engine, analyzer, uri, 7, 8);
    CHECK(has_label(in_block, "visible_top"));
    CHECK(has_label(in_block, "visible_block"));
    CHECK_FALSE(has_label(in_block, "hidden_other"));

    auto after_block = complete_at(engine, analyzer, uri, 9, 4);
    CHECK(has_label(after_block, "visible_top"));
    CHECK_FALSE(has_label(after_block, "visible_block"));
    CHECK_FALSE(has_label(after_block, "hidden_other"));
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

    // This mirrors editing below the final named-port connection in an instance:
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

TEST_CASE("completion: NamedPort uses cached project index for extra-file module", "[completion]") {
    CompletionEngine engine;
    Analyzer analyzer;

    const auto child_path =
        std::filesystem::temp_directory_path() / "completion_namedport_extra_child.sv";
    {
        std::ofstream out(child_path);
        REQUIRE(out.good());
        out << "module extra_child(input logic clk, output logic done);\n"
               "endmodule\n";
    }
    analyzer.set_extra_files({child_path.string()});
    analyzer.wait_for_background_index_idle();

    const std::string uri = "file:///tmp/completion_namedport_extra_top.sv";
    const std::string text =
        "module top;\n"
        "    extra_child u_child (.\n"
        "    );\n"
        "endmodule\n";
    analyzer.open(uri, text);

    auto result = complete_at(engine, analyzer, uri, 1, 26);
    CHECK(has_label(result, ".clk"));
    CHECK(has_label(result, ".done"));

    std::filesystem::remove(child_path);
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

TEST_CASE("completion: macros from extra files do not leak into unrelated files", "[completion]") {
    CompletionEngine engine;
    Analyzer analyzer;

    const auto extra = std::filesystem::temp_directory_path() / "completion_extra_macro.sv";
    {
        std::ofstream out(extra);
        REQUIRE(out.good());
        out << "`define EXTRA_UVM_STYLE_MACRO 1\n"
               "module extra_macro_owner;\n"
               "endmodule\n";
    }
    analyzer.set_extra_files({extra.string()});
    analyzer.wait_for_background_index_idle();

    const std::string uri = "file:///tmp/completion_macro_no_leak.sv";
    const std::string text =
        "`define LOCAL_MACRO 1\n"
        "module top;\n"
        "    logic a = `\n"
        "endmodule\n";
    analyzer.open(uri, text);

    auto result = complete_at(engine, analyzer, uri, 2, 15);

    CHECK(has_label(result, "LOCAL_MACRO"));
    CHECK_FALSE(has_label(result, "EXTRA_UVM_STYLE_MACRO"));
}

TEST_CASE("completion: repeated visible macros are deduplicated", "[completion]") {
    CompletionEngine engine;
    Analyzer analyzer;

    const auto extra = std::filesystem::temp_directory_path() / "completion_duplicate_macro.sv";
    {
        std::ofstream out(extra);
        REQUIRE(out.good());
        out << "`define DUPLICATE_VISIBLE_MACRO 1\n"
               "module duplicate_macro_extra;\n"
               "endmodule\n";
    }
    analyzer.set_extra_files({extra.string()});
    analyzer.wait_for_background_index_idle();

    const std::string uri = "file:///tmp/completion_duplicate_macro_current.sv";
    const std::string text =
        "`define DUPLICATE_VISIBLE_MACRO 2\n"
        "module top;\n"
        "    logic a = `\n"
        "    \n"
        "endmodule\n";
    analyzer.open(uri, text);

    auto macro_context = complete_at(engine, analyzer, uri, 2, 15);
    CHECK(count_label(macro_context, "DUPLICATE_VISIBLE_MACRO") == 1);

    auto identifier_context = complete_at(engine, analyzer, uri, 3, 4);
    CHECK(count_label(identifier_context, "`DUPLICATE_VISIBLE_MACRO") == 1);
}

TEST_CASE("completion: slang built-in macros are hidden", "[completion]") {
    CompletionEngine engine;
    Analyzer analyzer;

    const std::string uri = "file:///tmp/completion_builtin_macro_hidden.sv";
    analyzer.open(uri,
                  "module top;\n"
                  "    logic a = `\n"
                  "endmodule\n");

    auto without_user_define = complete_at(engine, analyzer, uri, 1, 15);
    CHECK_FALSE(has_label(without_user_define, "SV_COV_ERROR"));
    CHECK_FALSE(has_label(without_user_define, "`SV_COV_ERROR"));
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

TEST_CASE("completion: PackageScope filters symbols by declaring package", "[completion]") {
    CompletionEngine engine;
    Analyzer analyzer;
    const std::string uri = "file:///tmp/completion_pkgscope_filter.sv";
    const std::string text =
        "package pkg_a;\n"
        "    typedef enum logic [1:0] { A_IDLE, A_DONE } a_state_t;\n"
        "    class a_object;\n"
        "    endclass\n"
        "    int a_value;\n"
        "endpackage\n"
        "package pkg_b;\n"
        "    typedef logic [7:0] b_byte_t;\n"
        "    class b_object;\n"
        "    endclass\n"
        "    int b_value;\n"
        "endpackage\n"
        "module top;\n"
        "    pkg_a::\n"
        "endmodule\n";
    analyzer.open(uri, text);

    auto [line, col] = pos_of(text, "pkg_a::");
    auto result = complete_at(engine, analyzer, uri, line,
                              col + (int)std::string("pkg_a::").size());

    CHECK(has_label(result, "a_state_t"));
    CHECK(has_label(result, "A_IDLE"));
    CHECK(has_label(result, "A_DONE"));
    CHECK(has_label(result, "a_object"));
    CHECK(has_label(result, "a_value"));

    CHECK_FALSE(has_label(result, "b_byte_t"));
    CHECK_FALSE(has_label(result, "b_object"));
    CHECK_FALSE(has_label(result, "b_value"));
}

TEST_CASE("completion: package scope survives opening package definition file", "[completion]") {
    CompletionEngine engine;
    Analyzer analyzer;

    const auto lib = std::filesystem::temp_directory_path() / "completion_pkg_nav_lib.sv";
    const auto use = std::filesystem::temp_directory_path() / "completion_pkg_nav_use.sv";
    const std::string lib_text =
        "package nav_pkg;\n"
        "    typedef logic [7:0] nav_byte_t;\n"
        "    parameter int NAV_WIDTH = 8;\n"
        "endpackage\n";
    const std::string use_text =
        "module top;\n"
        "    initial begin\n"
        "        int value;\n"
        "        value = nav_pkg::\n"
        "    end\n"
        "endmodule\n";

    {
        std::ofstream out(lib);
        REQUIRE(out.good());
        out << lib_text;
    }
    {
        std::ofstream out(use);
        REQUIRE(out.good());
        out << use_text;
    }

    analyzer.set_extra_files({use.string(), lib.string()});
    analyzer.wait_for_background_index_idle();
    const std::string use_uri = "file://" + use.string();
    const std::string lib_uri = "file://" + lib.string();
    analyzer.open(use_uri, use_text);

    auto [line, col] = pos_of(use_text, "nav_pkg::");
    auto before_nav =
        complete_at(engine, analyzer, use_uri, line, col + (int)std::string("nav_pkg::").size());
    CHECK(has_label(before_nav, "nav_byte_t"));
    CHECK(has_label(before_nav, "NAV_WIDTH"));

    // Simulate go-to-definition opening the package file in the editor.  The
    // package file remains in the filelist, but its live open-buffer snapshot
    // should now be used by the shared extra-file snapshot.
    analyzer.open(lib_uri, lib_text);

    auto after_nav =
        complete_at(engine, analyzer, use_uri, line, col + (int)std::string("nav_pkg::").size());
    CHECK(has_label(after_nav, "nav_byte_t"));
    CHECK(has_label(after_nav, "NAV_WIDTH"));

    std::filesystem::remove(lib);
    std::filesystem::remove(use);
}

TEST_CASE("completion: project index shard follows live edits to extra file", "[completion]") {
    CompletionEngine engine;
    Analyzer analyzer;

    const auto lib = std::filesystem::temp_directory_path() / "completion_live_shard_lib.sv";
    const auto use = std::filesystem::temp_directory_path() / "completion_live_shard_use.sv";
    const std::string old_lib_text =
        "package old_live_pkg;\n"
        "    parameter int OLD_VALUE = 1;\n"
        "endpackage\n";
    const std::string new_lib_text =
        "package new_live_pkg;\n"
        "    parameter int NEW_VALUE = 2;\n"
        "endpackage\n";
    const std::string use_text =
        "module top;\n"
        "    new_live_pkg::\n"
        "endmodule\n";

    {
        std::ofstream out(lib);
        REQUIRE(out.good());
        out << old_lib_text;
    }
    {
        std::ofstream out(use);
        REQUIRE(out.good());
        out << use_text;
    }

    analyzer.set_extra_files({use.string(), lib.string()});
    analyzer.wait_for_background_index_idle();
    const std::string use_uri = "file://" + use.string();
    const std::string lib_uri = "file://" + lib.string();
    analyzer.open(use_uri, use_text);

    // Opening/changing the library file replaces only that file's project-index
    // shard.  The stale old_live_pkg symbols from the original disk parse
    // should not survive in project-aware completion.
    analyzer.open(lib_uri, new_lib_text);

    auto [line, col] = pos_of(use_text, "new_live_pkg::");
    auto result = complete_at(engine, analyzer, use_uri, line,
                              col + (int)std::string("new_live_pkg::").size());
    CHECK(has_label(result, "NEW_VALUE"));
    CHECK_FALSE(has_label(result, "OLD_VALUE"));

    const std::string stale_query_text =
        "module top;\n"
        "    old_live_pkg::\n"
        "endmodule\n";
    analyzer.change(use_uri, stale_query_text);
    auto [old_line, old_col] = pos_of(stale_query_text, "old_live_pkg::");
    auto stale_result = complete_at(engine, analyzer, use_uri, old_line,
                                    old_col + (int)std::string("old_live_pkg::").size());
    CHECK_FALSE(has_label(stale_result, "OLD_VALUE"));

    std::filesystem::remove(lib);
    std::filesystem::remove(use);
}

TEST_CASE("completion: identifier completion requires package import", "[completion]") {
    CompletionEngine engine;
    Analyzer analyzer;
    const std::string uri = "file:///tmp/completion_import_filter.sv";
    const std::string text =
        "package pkg_no_import;\n"
        "    class hidden_class;\n"
        "    endclass\n"
        "    typedef enum { H_IDLE, H_DONE } hidden_state_t;\n"
        "endpackage\n"
        "module top_no_import;\n"
        "    \n"
        "endmodule\n"
        "import pkg_no_import::hidden_class;\n"
        "module top_explicit_import;\n"
        "    \n"
        "endmodule\n"
        "import pkg_no_import::*;\n"
        "module top_wildcard_import;\n"
        "    \n"
        "endmodule\n";
    analyzer.open(uri, text);

    auto no_import = complete_at(engine, analyzer, uri, 6, 4);
    CHECK(has_label(no_import, "pkg_no_import"));
    CHECK_FALSE(has_label(no_import, "hidden_class"));
    CHECK_FALSE(has_label(no_import, "hidden_state_t"));
    CHECK_FALSE(has_label(no_import, "H_IDLE"));

    auto explicit_import = complete_at(engine, analyzer, uri, 10, 4);
    CHECK(has_label(explicit_import, "hidden_class"));
    CHECK_FALSE(has_label(explicit_import, "hidden_state_t"));
    CHECK_FALSE(has_label(explicit_import, "H_IDLE"));

    auto wildcard_import = complete_at(engine, analyzer, uri, 14, 4);
    CHECK(has_label(wildcard_import, "hidden_class"));
    CHECK(has_label(wildcard_import, "hidden_state_t"));
    CHECK(has_label(wildcard_import, "H_IDLE"));
}

TEST_CASE("completion: imports from extra files do not leak into current file", "[completion]") {
    CompletionEngine engine;
    Analyzer analyzer;

    const auto extra = std::filesystem::temp_directory_path() / "completion_extra_import.sv";
    {
        std::ofstream out(extra);
        REQUIRE(out.good());
        out << "package pkg_extra_import;\n"
               "    typedef enum { EXTRA_IDLE, EXTRA_DONE } extra_state_t;\n"
               "    parameter int EXTRA_PARAM = 1;\n"
               "endpackage\n"
               "import pkg_extra_import::*;\n"
               "module extra_import_user;\n"
               "endmodule\n";
    }
    analyzer.set_extra_files({extra.string()});
    analyzer.wait_for_background_index_idle();

    const std::string uri = "file:///tmp/completion_import_no_leak.sv";
    const std::string text =
        "module top_no_import;\n"
        "    \n"
        "endmodule\n";
    analyzer.open(uri, text);

    auto result = complete_at(engine, analyzer, uri, 1, 4);

    // Generic identifier completion is intentionally current-file-only on the
    // hot typing path.  The package can still be reached through explicit
    // package-scope completion (`pkg_extra_import::`), but it should not force
    // project-wide .f index merging for every ordinary identifier request.
    CHECK_FALSE(has_label(result, "pkg_extra_import"));
    CHECK_FALSE(has_label(result, "extra_state_t"));
    CHECK_FALSE(has_label(result, "EXTRA_IDLE"));
    CHECK_FALSE(has_label(result, "EXTRA_PARAM"));
}

TEST_CASE("completion: current file listed in extra files is not duplicated", "[completion]") {
    CompletionEngine engine;
    Analyzer analyzer;

    const auto path = std::filesystem::temp_directory_path() / "completion_no_self_merge.sv";
    const std::string text =
        "module top_no_self_merge;\n"
        "    logic clk;\n"
        "    \n"
        "endmodule\n";
    {
        std::ofstream out(path);
        REQUIRE(out.good());
        out << text;
    }
    analyzer.set_extra_files({path.string()});
    analyzer.wait_for_background_index_idle();

    const std::string uri = "file://" + path.string();
    analyzer.open(uri, text);

    auto result = complete_at(engine, analyzer, uri, 2, 4);

    CHECK(count_label(result, "clk") == 1);
    CHECK(count_label(result, "top_no_self_merge") == 1);
}

TEST_CASE("completion: module names are hidden in procedural context", "[completion]") {
    CompletionEngine engine;
    Analyzer analyzer;
    const std::string uri = "file:///tmp/completion_module_context.sv";
    const std::string text =
        "module child_for_inst;\n"
        "endmodule\n"
        "module top;\n"
        "    logic data;\n"
        "    \n"
        "    always_comb begin\n"
        "        data = \n"
        "    end\n"
        "endmodule\n";
    analyzer.open(uri, text);

    auto module_item = complete_at(engine, analyzer, uri, 4, 4);
    CHECK(has_label(module_item, "child_for_inst"));

    auto procedural = complete_at(engine, analyzer, uri, 6, 15);
    CHECK(has_label(procedural, "data"));
    CHECK_FALSE(has_label(procedural, "child_for_inst"));
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

TEST_CASE("completion: UVM-like package scope returns indexed symbols", "[completion]") {
    CompletionEngine engine;
    Analyzer analyzer;

    const auto setup = make_synthetic_uvm_fixture("uvm_pkg_scope_basic");
    configure_synthetic_uvm_index(analyzer, setup);

    const std::string uri = "file:///tmp/completion_uvm_pkg_scope_basic.sv";
    const std::string text =
        "module top;\n"
        "    initial begin\n"
        "        uvm_pkg::\n"
        "    end\n"
        "endmodule\n";
    analyzer.open(uri, text);

    auto [line, col] = pos_after(text, "        uvm_pkg::");
    auto result = complete_at(engine, analyzer, uri, line, col);

    CHECK(has_label(result, "uvm_object"));
    CHECK(has_label(result, "uvm_component"));
}

TEST_CASE("completion: UVM-like macro prefix returns macros from included headers", "[completion]") {
    CompletionEngine engine;
    Analyzer analyzer;

    const auto setup = make_synthetic_uvm_fixture("uvm_macro_prefix");
    configure_synthetic_uvm_index(analyzer, setup);

    const std::string uri = "file:///tmp/completion_uvm_macro_prefix.sv";
    const std::string text =
        "`include \"uvm_macros.svh\"\n"
        "import uvm_pkg::*;\n"
        "class local_item extends uvm_object;\n"
        "  `uvm_\n"
        "endclass\n";
    analyzer.open(uri, text);

    // UVM macros are not package members, so this fixture includes the synthetic
    // UVM macro header explicitly and relies on the synthetic include directory
    // to resolve it.  Completing after a typed backtick prefix must then surface
    // those macros as ordinary macro completions, without mixing in UVM classes.
    auto [line, col] = pos_after(text, "  `uvm_");
    auto result = complete_at(engine, analyzer, uri, line, col);

    CHECK(has_label(result, "uvm_object_utils"));
    CHECK(has_label(result, "uvm_component_utils"));
    CHECK(has_label(result, "uvm_info"));
    CHECK(has_label(result, "uvm_warning"));
    CHECK(has_label(result, "uvm_error"));
    CHECK(has_label(result, "uvm_fatal"));
    CHECK_FALSE(has_label(result, "uvm_object"));
}

TEST_CASE("completion: UVM-like package scope returns expected package symbols", "[completion]") {
    CompletionEngine engine;
    Analyzer analyzer;

    const auto setup = make_synthetic_uvm_fixture("uvm_pkg_scope_expected");
    configure_synthetic_uvm_index(analyzer, setup);

    const std::string uri = "file:///tmp/completion_uvm_package_scope_expected.sv";
    const std::string text =
        "module top;\n"
        "    initial begin\n"
        "        uvm_pkg::\n"
        "    end\n"
        "endmodule\n";
    analyzer.open(uri, text);

    auto [line, col] = pos_after(text, "        uvm_pkg::");
    auto result = complete_at(engine, analyzer, uri, line, col);

    CHECK(has_label(result, "uvm_object"));
    CHECK(has_label(result, "uvm_component"));
    CHECK(has_label(result, "uvm_sequence_item"));
    CHECK(has_label(result, "uvm_env"));
    CHECK(has_label(result, "uvm_phase"));
    CHECK(has_label(result, "uvm_config_db"));
}

TEST_CASE("completion: UVM-like parameterized class scope returns static methods", "[completion]") {
    CompletionEngine engine;
    Analyzer analyzer;

    const auto setup = make_synthetic_uvm_fixture("uvm_config_db_static");
    configure_synthetic_uvm_index(analyzer, setup);

    const std::string uri = "file:///tmp/completion_uvm_config_db_static.sv";
    const std::string text =
        "import uvm_pkg::*;\n"
        "module top;\n"
        "    initial begin\n"
        "        uvm_config_db#(uvm_object)::\n"
        "    end\n"
        "endmodule\n";
    analyzer.open(uri, text);

    // uvm_config_db is a parameterized class in the indexed UVM package.  The
    // completion engine should resolve the package class, ignore the concrete
    // type parameter for lookup purposes, and then return static class methods.
    auto [line, col] = pos_after(text, "        uvm_config_db#(uvm_object)::");
    auto result = complete_at(engine, analyzer, uri, line, col);

    CHECK(has_label(result, "get"));
    CHECK(has_label(result, "set"));
    CHECK(has_label(result, "exists"));
    CHECK(has_label(result, "wait_modified"));
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

TEST_CASE("completion: MemberAccess includes inherited class members", "[completion]") {
    CompletionEngine engine;
    Analyzer analyzer;
    const std::string uri = "file:///tmp/completion_member_inherited.sv";
    const std::string text =
        "class base_cfg;\n"
        "    int base_depth;\n"
        "    function void base_apply();\n"
        "    endfunction\n"
        "endclass\n"
        "class child_cfg extends base_cfg;\n"
        "    int child_depth;\n"
        "endclass\n"
        "module top;\n"
        "    child_cfg cfg;\n"
        "    initial cfg.\n"
        "endmodule\n";
    analyzer.open(uri, text);

    auto result = complete_at(engine, analyzer, uri, 10, 16);
    CHECK(has_label(result, "base_depth"));
    CHECK(has_label(result, "base_apply"));
    CHECK(has_label(result, "child_depth"));
}

TEST_CASE("completion: MemberAccess includes interface signals and modports", "[completion]") {
    CompletionEngine engine;
    Analyzer analyzer;
    const std::string uri = "file:///tmp/completion_member_interface.sv";
    const std::string text =
        "interface bus_if;\n"
        "    logic valid;\n"
        "    logic ready;\n"
        "    modport master(output valid, input ready);\n"
        "endinterface\n"
        "module top;\n"
        "    bus_if bus();\n"
        "    initial bus.\n"
        "endmodule\n";
    analyzer.open(uri, text);

    auto result = complete_at(engine, analyzer, uri, 7, 16);
    CHECK(has_label(result, "valid"));
    CHECK(has_label(result, "ready"));
    CHECK(has_label(result, "master"));
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

TEST_CASE("completion: generic identifier completion avoids extra-file modules", "[completion]") {
    const auto extra_path =
        std::filesystem::temp_directory_path() / "completion_scope_extra.sv";
    {
        std::ofstream out(extra_path);
        out << "module m_extra_adder(input logic i_a);\nendmodule\n";
    }

    CompletionEngine engine;
    Analyzer analyzer;
    analyzer.set_extra_files({extra_path.string()});
    analyzer.wait_for_background_index_idle();

    const std::string uri = "file:///tmp/completion_scope_local.sv";
    // m_local_adder declared in the open document
    const std::string text =
        "module m_local_adder(input logic i_a);\nendmodule\n"
        "module top;\n"
        "    m\n"
        "endmodule\n";
    analyzer.open(uri, text);

    auto result = complete_at(engine, analyzer, uri, 3, 5);

    // Generic identifier completion should stay local.  Extra-file modules are
    // used by narrower project-wide contexts such as named-port completion, not
    // by every ordinary identifier popup while typing on HPC/shared filesystems.
    REQUIRE(has_label(result, "m_local_adder"));
    CHECK_FALSE(has_label(result, "m_extra_adder"));

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
    analyzer.wait_for_background_index_idle();

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
