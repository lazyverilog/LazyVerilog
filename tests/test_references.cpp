#include "analyzer.hpp"
#include "features/references.hpp"
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>

namespace {

static std::pair<int, int> find_position(std::string_view text, std::string_view needle) {
    const size_t pos = text.find(needle);
    REQUIRE(pos != std::string_view::npos);

    int line = 0;
    int col = 0;
    for (size_t i = 0; i < pos; ++i) {
        if (text[i] == '\n') {
            ++line;
            col = 0;
        } else {
            ++col;
        }
    }
    return {line, col};
}

static std::pair<int, int> find_position_after(std::string_view text, std::string_view needle,
                                               std::string_view after) {
    const size_t after_pos = text.find(after);
    REQUIRE(after_pos != std::string_view::npos);
    const size_t pos = text.find(needle, after_pos + after.size());
    REQUIRE(pos != std::string_view::npos);

    int line = 0;
    int col = 0;
    for (size_t i = 0; i < pos; ++i) {
        if (text[i] == '\n') {
            ++line;
            col = 0;
        } else {
            ++col;
        }
    }
    return {line, col};
}

} // namespace

TEST_CASE("references: verifies tokens against the same syntax-tree definition", "[references]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/references_fixture.sv";
    analyzer.open(uri, R"(
module top;
    logic a;
    logic b;
    assign a = a;
    assign b = a;
endmodule
)");

    TextDocumentReferences::Params params;
    params.textDocument.uri.raw_uri_ = uri;
    params.position = lsPosition(4, 11);
    params.context.includeDeclaration = true;

    auto refs = provide_references(analyzer, params);
    REQUIRE(refs.size() == 4);
    CHECK(refs[0].range.start.line == 2);
    CHECK(refs[0].range.start.character == 10);
    CHECK(refs[1].range.start.line == 4);
    CHECK(refs[2].range.start.line == 4);
    CHECK(refs[3].range.start.line == 5);

    params.context.includeDeclaration = false;
    refs = provide_references(analyzer, params);
    REQUIRE(refs.size() == 3);
    CHECK(refs[0].range.start.line == 4);
}

TEST_CASE("references: module declaration finds open cross-file instantiations by symbol id",
          "[references]") {
    const std::string memory = R"(module memory(input logic clk);
endmodule
)";
    const std::string top = R"(module memory_top;
    memory u_mem0(.clk(clk));
    memory u_mem1(.clk(clk));
endmodule
)";

    const auto memory_path = std::filesystem::temp_directory_path() /
                             "lazyverilog_refs_symbolid_memory.sv";
    const auto top_path =
        std::filesystem::temp_directory_path() / "lazyverilog_refs_symbolid_top.sv";
    {
        std::ofstream out(memory_path);
        REQUIRE(out.good());
        out << memory;
    }
    {
        std::ofstream out(top_path);
        REQUIRE(out.good());
        out << top;
    }

    Analyzer analyzer;
    analyzer.set_extra_files({memory_path.string(), top_path.string()});

    const std::string memory_uri = "file://" + memory_path.string();
    const std::string top_uri = "file://" + top_path.string();
    analyzer.open(memory_uri, memory);
    analyzer.open(top_uri, top);

    const auto [line, col] = find_position(memory, "memory");
    const auto refs = analyzer.find_references(memory_uri, line, col, true);

    REQUIRE(refs.size() == 3);
    CHECK(refs[0].uri == memory_uri);
    CHECK(refs[0].line == 0);
    CHECK(refs[0].col == 7);
    CHECK(refs[1].uri == top_uri);
    CHECK(refs[1].line == 1);
    CHECK(refs[1].col == 4);
    CHECK(refs[2].uri == top_uri);
    CHECK(refs[2].line == 2);
    CHECK(refs[2].col == 4);

    std::filesystem::remove(memory_path);
    std::filesystem::remove(top_path);
}

TEST_CASE("references: generic local does not fall back to closed project name matches",
          "[references]") {
    const std::string top = R"(module top;
    typedef enum logic [1:0] { IDLE, BUSY } state_t;
    state_t state;
    always_comb state = BUSY;
endmodule
)";
    const std::string unrelated = R"(package unrelated_pkg;
    int state;
    function int get_state();
        return state;
    endfunction
endpackage
)";

    const auto unrelated_path =
        std::filesystem::temp_directory_path() / "lazyverilog_refs_unrelated_state.sv";
    {
        std::ofstream out(unrelated_path);
        REQUIRE(out.good());
        out << unrelated;
    }

    Analyzer analyzer;
    analyzer.set_extra_files({unrelated_path.string()});
    const std::string top_uri = "file:///tmp/lazyverilog_refs_local_state_top.sv";
    analyzer.open(top_uri, top);

    const auto [line, col] = find_position(top, "state;");
    const auto refs = analyzer.find_references(top_uri, line, col, true);

    REQUIRE(refs.size() == 2);
    for (const auto& ref : refs)
        CHECK(ref.uri == top_uri);

    std::filesystem::remove(unrelated_path);
}

TEST_CASE("references: module-signal SymbolID separates same signal name across open modules",
          "[references]") {
    const std::string text = R"(module top_a;
    logic state;
    always_comb state = 1'b0;
endmodule

module top_b;
    logic state;
    always_comb state = 1'b1;
endmodule
)";

    Analyzer analyzer;
    const std::string uri = "file:///tmp/lazyverilog_refs_open_module_signal_scope.sv";
    analyzer.open(uri, text);

    const auto [line, col] = find_position(text, "state;");
    const auto refs = analyzer.find_references(uri, line, col, true);

    REQUIRE(refs.size() == 2);
    CHECK(refs[0].uri == uri);
    CHECK(refs[0].line == 1);
    CHECK(refs[0].col == 10);
    CHECK(refs[1].uri == uri);
    CHECK(refs[1].line == 2);
    CHECK(refs[1].col == 16);
}

TEST_CASE("references: module-signal SymbolID excludes same-name closed project signals",
          "[references]") {
    const std::string current = R"(module top_a;
    logic state;
    always_comb state = 1'b0;
endmodule
)";
    const std::string closed = R"(module top_b;
    logic state;
    always_comb state = 1'b1;
endmodule
)";

    const auto closed_path =
        std::filesystem::temp_directory_path() / "lazyverilog_refs_closed_module_state.sv";
    {
        std::ofstream out(closed_path);
        REQUIRE(out.good());
        out << closed;
    }

    Analyzer analyzer;
    analyzer.set_extra_files({closed_path.string()});
    const std::string uri = "file:///tmp/lazyverilog_refs_current_module_state.sv";
    analyzer.open(uri, current);

    const auto [line, col] = find_position(current, "state;");
    const auto refs = analyzer.find_references(uri, line, col, true);

    REQUIRE(refs.size() == 2);
    for (const auto& ref : refs)
        CHECK(ref.uri == uri);

    std::filesystem::remove(closed_path);
}

TEST_CASE("references: typedef struct fields with same name stay separate", "[references]") {
    const std::string text = R"(typedef struct {
    logic [7:0] addr;
    logic       valid;
} packet_wo_data_t;

typedef struct {
    logic signed [7:0] addr;
    logic        [31:0] data;
    logic               valid;
} packet_ta;
)";

    Analyzer analyzer;
    const std::string uri = "file:///tmp/lazyverilog_refs_typedef_struct_fields.sv";
    analyzer.open(uri, text);

    const auto [wo_line, wo_col] = find_position(text, "addr;");
    auto refs = analyzer.find_references(uri, wo_line, wo_col, true);
    REQUIRE(refs.size() == 1);
    CHECK(refs[0].uri == uri);
    CHECK(refs[0].line == 1);
    CHECK(refs[0].col == 16);

    const auto [packet_ta_line, packet_ta_col] = find_position_after(text, "addr;", "} packet_wo_data_t;");
    refs = analyzer.find_references(uri, packet_ta_line, packet_ta_col, true);
    REQUIRE(refs.size() == 1);
    CHECK(refs[0].uri == uri);
    CHECK(refs[0].line == 6);
    CHECK(refs[0].col == 23);
}

TEST_CASE("references: enum members include declaration and uses", "[references]") {
    const std::string text = R"(typedef enum logic [1:0] {
    IDLE,
    BUSY
} state_t;

module top;
    state_t state;
    always_comb begin
        state = BUSY;
    end
endmodule
)";

    Analyzer analyzer;
    const std::string uri = "file:///tmp/lazyverilog_refs_enum_members.sv";
    analyzer.open(uri, text);

    const auto [line, col] = find_position(text, "BUSY");
    const auto refs = analyzer.find_references(uri, line, col, true);

    REQUIRE(refs.size() == 2);
    CHECK(refs[0].uri == uri);
    CHECK(refs[0].line == 2);
    CHECK(refs[0].col == 4);
    CHECK(refs[1].uri == uri);
    CHECK(refs[1].line == 8);
    CHECK(refs[1].col == 16);
}

TEST_CASE("references: struct and union member accesses use typedef-field SymbolID",
          "[references]") {
    const std::string text = R"(typedef struct {
    logic [7:0] addr;
    logic       valid;
} packet_t;

typedef union {
    logic [7:0] addr;
    logic [7:0] code;
} choice_t;

module top;
    packet_t pkt;
    choice_t choice;
    always_comb begin
        pkt.addr = '0;
        choice.addr = '0;
    end
endmodule
)";

    Analyzer analyzer;
    const std::string uri = "file:///tmp/lazyverilog_refs_struct_union_members.sv";
    analyzer.open(uri, text);

    auto [line, col] = find_position_after(text, "addr", "pkt.");
    auto refs = analyzer.find_references(uri, line, col, true);
    REQUIRE(refs.size() == 2);
    CHECK(refs[0].uri == uri);
    CHECK(refs[0].line == 1);
    CHECK(refs[0].col == 16);
    CHECK(refs[1].uri == uri);
    CHECK(refs[1].line == 14);
    CHECK(refs[1].col == 12);

    auto choice_pos = find_position_after(text, "addr", "choice.");
    line = choice_pos.first;
    col = choice_pos.second;
    refs = analyzer.find_references(uri, line, col, true);
    REQUIRE(refs.size() == 2);
    CHECK(refs[0].uri == uri);
    CHECK(refs[0].line == 6);
    CHECK(refs[0].col == 16);
    CHECK(refs[1].uri == uri);
    CHECK(refs[1].line == 15);
    CHECK(refs[1].col == 15);
}

TEST_CASE("references: included struct field use survives duplicate object declaration",
          "[references]") {
    const auto dir = std::filesystem::temp_directory_path() / "lazyverilog_refs_include_struct_field";
    std::filesystem::create_directories(dir);

    const auto header_path = dir / "params.svh";
    const auto top_path = dir / "top.sv";

    const std::string header = R"(typedef struct packed {
    logic valid;
    logic [3:0] id;
    logic [31:0] data;
} fifo_entry_t;
)";
    const std::string top = R"(`include "params.svh"
module top(output logic test);
    fifo_entry_t test;
    always_comb begin
        test.id = 1;
    end
endmodule
)";

    {
        std::ofstream out(header_path);
        REQUIRE(out.good());
        out << header;
    }
    {
        std::ofstream out(top_path);
        REQUIRE(out.good());
        out << top;
    }

    Analyzer analyzer;
    analyzer.set_include_dirs({dir.string()});
    analyzer.set_extra_files({top_path.string()});

    const std::string header_uri = "file://" + header_path.string();
    analyzer.open(header_uri, header);

    const auto [line, col] = find_position_after(header, "id;", "[3:0]");
    const auto refs = analyzer.find_references(header_uri, line, col, true);

    REQUIRE(refs.size() == 2);
    CHECK(refs[0].uri == header_uri);
    CHECK(refs[0].line == 2);
    CHECK(refs[0].col == 16);
    CHECK(refs[1].uri == "file://" + top_path.string());
    CHECK(refs[1].line == 4);
    CHECK(refs[1].col == 13);

    std::filesystem::remove(top_path);
    std::filesystem::remove(header_path);
    std::filesystem::remove(dir);
}

TEST_CASE("references: included-header occurrences keep their actual source URI",
          "[references]") {
    const auto dir = std::filesystem::temp_directory_path() / "lazyverilog_refs_include_uri";
    std::filesystem::create_directories(dir);

    const auto header_path = dir / "params.svh";
    const auto closed_path = dir / "closed.sv";
    const auto top_path = dir / "top.sv";

    const std::string header = R"(task add_number(
    input int a,
    input int b,
    output int result
);
    result = a + b;
endtask
)";
    // Line 0 intentionally contains a different identifier.  Before reference
    // entries carried their actual URI, a token from params.svh line 0 could be
    // reported against this parsed shard and appear to point at `address`.
    const std::string closed = R"(`include "params.svh"
module closed;
    input int address;
endmodule
)";
    const std::string top = R"(`include "params.svh"
module top;
    initial add_number();
endmodule
)";

    {
        std::ofstream out(header_path);
        REQUIRE(out.good());
        out << header;
    }
    {
        std::ofstream out(closed_path);
        REQUIRE(out.good());
        out << closed;
    }
    {
        std::ofstream out(top_path);
        REQUIRE(out.good());
        out << top;
    }

    Analyzer analyzer;
    analyzer.set_include_dirs({dir.string()});
    analyzer.set_extra_files({closed_path.string()});

    const auto snapshots = analyzer.extra_index_snapshots();
    REQUIRE(snapshots.size() == 1);
    const auto indexed_add_number = std::find_if(
        snapshots[0].index.references.begin(), snapshots[0].index.references.end(),
        [&](const ReferenceEntry& ref) {
            return ref.name == "add_number" &&
                   snapshots[0].index.source_uri(ref.file_id) == "file://" + header_path.string();
        });
    REQUIRE(indexed_add_number != snapshots[0].index.references.end());

    const std::string top_uri = "file://" + top_path.string();
    const std::string closed_uri = "file://" + closed_path.string();
    analyzer.open(top_uri, top);

    const auto [line, col] = find_position(top, "add_number();");
    const auto refs = analyzer.find_references(top_uri, line, col, true);

    REQUIRE(refs.size() == 1);
    CHECK(refs[0].uri == top_uri);
    CHECK(refs[0].line == 2);
    CHECK(refs[0].col == 12);
    for (const auto& ref : refs)
        CHECK(ref.uri != closed_uri);

    std::filesystem::remove(top_path);
    std::filesystem::remove(closed_path);
    std::filesystem::remove(header_path);
    std::filesystem::remove(dir);
}

TEST_CASE("references: included typedef declaration keeps header URI", "[references]") {
    const auto dir = std::filesystem::temp_directory_path() / "lazyverilog_refs_include_typedef";
    std::filesystem::create_directories(dir);

    const auto header_path = dir / "params.svh";
    const auto includer_path = dir / "memory.sv";

    const std::string header = R"(typedef enum logic [1:0] {
    IDLE,
    FETCH,
    EXECUTE,
    ERROR
} state_t;
)";
    const std::string includer = R"(`include "params.svh"
module memory;
    logic unrelated;
endmodule
)";

    {
        std::ofstream out(header_path);
        REQUIRE(out.good());
        out << header;
    }
    {
        std::ofstream out(includer_path);
        REQUIRE(out.good());
        out << includer;
    }

    Analyzer analyzer;
    analyzer.set_include_dirs({dir.string()});
    analyzer.set_extra_files({includer_path.string()});

    const std::string header_uri = "file://" + header_path.string();
    const std::string includer_uri = "file://" + includer_path.string();
    analyzer.open(header_uri, header);

    const auto snapshots = analyzer.extra_index_snapshots();
    REQUIRE(snapshots.size() == 1);
    const auto indexed_state_t = std::find_if(
        snapshots[0].index.references.begin(), snapshots[0].index.references.end(),
        [&](const ReferenceEntry& ref) {
            return ref.name == "state_t" &&
                   snapshots[0].index.source_uri(ref.file_id) == header_uri &&
                   ref.symbol_debug == "typedef::state_t";
        });
    REQUIRE(indexed_state_t != snapshots[0].index.references.end());

    const auto [line, col] = find_position(header, "} state_t;");
    const auto refs = analyzer.find_references(header_uri, line, col + 2, true);

    REQUIRE(refs.size() == 1);
    CHECK(refs[0].uri == header_uri);
    CHECK(refs[0].line == 5);
    CHECK(refs[0].col == 2);
    CHECK(refs[0].uri != includer_uri);

    std::filesystem::remove(includer_path);
    std::filesystem::remove(header_path);
    std::filesystem::remove(dir);
}
