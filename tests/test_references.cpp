#include "analyzer.hpp"
#include "features/references.hpp"
#include "string_utils.hpp"
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <set>
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

static std::filesystem::path write_temp_sv(const std::string& name, const std::string& text) {
    auto path = std::filesystem::temp_directory_path() / name;
    std::ofstream out(path);
    REQUIRE(out.good());
    out << text;
    out.close();
    return path;
}

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

TEST_CASE("references: macro invocation resolves through macro SymbolID", "[references]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/references_macro_fixture.sv";
    const std::string text = R"(`define WIDTH 8

module top;
    logic [`WIDTH-1:0] a;
    logic [`WIDTH-1:0] b;
endmodule
)";
    analyzer.open(uri, text);

    auto [line, col] = find_position_after(text, "WIDTH", "logic [");
    const auto refs = analyzer.find_references(uri, line, col, true);

    REQUIRE(refs.size() == 3);
    CHECK(refs[0].line == 0);
    CHECK(refs[0].col == 8);
    CHECK(refs[1].line == 3);
    CHECK(refs[1].col == 12);
    CHECK(refs[2].line == 4);
    CHECK(refs[2].col == 12);
}

TEST_CASE("references: macro definition finds macro invocations", "[references]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/references_macro_definition_fixture.sv";
    const std::string text = R"(`define FOO 1

module top;
    localparam int A = `FOO;
    localparam int B = `FOO;
endmodule
)";
    analyzer.open(uri, text);

    auto [line, col] = find_position(text, "FOO");
    const auto refs = analyzer.find_references(uri, line, col, false);

    REQUIRE(refs.size() == 2);
    CHECK(refs[0].line == 3);
    CHECK(refs[0].col == 24);
    CHECK(refs[1].line == 4);
    CHECK(refs[1].col == 24);
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
    analyzer.wait_for_background_index_idle();

    const std::string memory_uri = uri_from_path(memory_path);
    const std::string top_uri = uri_from_path(top_path);
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
    analyzer.wait_for_background_index_idle();
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
    analyzer.wait_for_background_index_idle();
    const std::string uri = "file:///tmp/lazyverilog_refs_current_module_state.sv";
    analyzer.open(uri, current);

    const auto [line, col] = find_position(current, "state;");
    const auto refs = analyzer.find_references(uri, line, col, true);

    REQUIRE(refs.size() == 2);
    for (const auto& ref : refs)
        CHECK(ref.uri == uri);

    std::filesystem::remove(closed_path);
}

TEST_CASE("references: module-local subroutine excludes same-name closed project subroutine",
          "[references]") {
    const std::string current = R"(module top;
    function int calc();
        return 1;
    endfunction

    initial begin
        int value = calc();
    end
endmodule
)";
    const std::string closed = R"(function int calc();
    return 2;
endfunction

module other;
    initial begin
        int value = calc();
    end
endmodule
)";

    const auto closed_path =
        std::filesystem::temp_directory_path() / "lazyverilog_refs_closed_global_calc.sv";
    {
        std::ofstream out(closed_path);
        REQUIRE(out.good());
        out << closed;
    }

    Analyzer analyzer;
    analyzer.set_extra_files({closed_path.string()});
    analyzer.wait_for_background_index_idle();
    const std::string uri = "file:///tmp/lazyverilog_refs_current_local_calc.sv";
    analyzer.open(uri, current);

    const auto [line, col] = find_position_after(current, "calc", "function int");
    const auto refs = analyzer.find_references(uri, line, col, true);

    REQUIRE(refs.size() == 2);
    for (const auto& ref : refs)
        CHECK(ref.uri == uri);
    CHECK(refs[0].line == 1);
    CHECK(refs[0].col == 17);
    CHECK(refs[1].line == 6);
    CHECK(refs[1].col == 20);

    std::filesystem::remove(closed_path);
}

TEST_CASE("references: module-local subroutine stays scoped when current file is indexed",
          "[references]") {
    const auto dir = std::filesystem::temp_directory_path() / "lazyverilog_refs_module_subroutine";
    std::filesystem::create_directories(dir);

    const auto top_path = dir / "top.sv";
    const auto other_path = dir / "other.sv";

    const std::string top = R"(module top;
    task add_number();
    endtask

    initial begin
        add_number();
    end
endmodule
)";
    const std::string other = R"(task add_number();
endtask

module other;
    task add_number();
    endtask

    initial add_number();
endmodule
)";

    {
        std::ofstream out(top_path);
        REQUIRE(out.good());
        out << top;
    }
    {
        std::ofstream out(other_path);
        REQUIRE(out.good());
        out << other;
    }

    Analyzer analyzer;
    // Include the open file in the project snapshot on purpose.  This mirrors a
    // real filelist-backed LSP session, where the current buffer is open but the
    // same disk file can also have a published background index shard.  The
    // reference engine must therefore use the owner-qualified subroutine ID
    // (`module_subroutine::top::add_number`), not the weak textual
    // `name:add_number` fallback from the project shard.
    analyzer.set_extra_files({top_path.string(), other_path.string()});
    analyzer.wait_for_background_index_idle();

    const std::string top_uri = uri_from_path(top_path);
    analyzer.open(top_uri, top);

    const auto [line, col] = find_position(top, "add_number();");
    const auto refs = analyzer.find_references(top_uri, line, col, true);

    REQUIRE(refs.size() == 2);
    CHECK(refs[0].uri == top_uri);
    CHECK(refs[0].line == 1);
    CHECK(refs[0].col == 9);
    CHECK(refs[1].uri == top_uri);
    CHECK(refs[1].line == 5);
    CHECK(refs[1].col == 8);

    std::filesystem::remove(top_path);
    std::filesystem::remove(other_path);
    std::filesystem::remove(dir);
}

TEST_CASE("references: package-local subroutine excludes same-name package subroutine",
          "[references]") {
    const auto dir = std::filesystem::temp_directory_path() / "lazyverilog_refs_package_subroutine";
    std::filesystem::create_directories(dir);

    const auto p_path = dir / "p.sv";
    const auto q_path = dir / "q.sv";

    const std::string p = R"(package p;
    task add_number();
    endtask

    function void use();
        add_number();
    endfunction
endpackage
)";
    const std::string q = R"(package q;
    task add_number();
    endtask

    function void use();
        add_number();
    endfunction
endpackage
)";

    {
        std::ofstream out(p_path);
        REQUIRE(out.good());
        out << p;
    }
    {
        std::ofstream out(q_path);
        REQUIRE(out.good());
        out << q;
    }

    Analyzer analyzer;
    analyzer.set_extra_files({p_path.string(), q_path.string()});
    analyzer.wait_for_background_index_idle();

    const std::string p_uri = uri_from_path(p_path);
    analyzer.open(p_uri, p);

    const auto [line, col] = find_position(p, "add_number();");
    const auto refs = analyzer.find_references(p_uri, line, col, true);

    REQUIRE(refs.size() == 2);
    for (const auto& ref : refs)
        CHECK(ref.uri == p_uri);
    CHECK(refs[0].line == 1);
    CHECK(refs[0].col == 9);
    CHECK(refs[1].line == 5);
    CHECK(refs[1].col == 8);

    std::filesystem::remove(p_path);
    std::filesystem::remove(q_path);
    std::filesystem::remove(dir);
}

TEST_CASE("references: class method excludes same-name method in another class", "[references]") {
    const auto dir = std::filesystem::temp_directory_path() / "lazyverilog_refs_class_method";
    std::filesystem::create_directories(dir);

    const auto a_path = dir / "a.sv";
    const auto b_path = dir / "b.sv";

    const std::string a = R"(class A;
    function int calc();
        return 1;
    endfunction

    function void use();
        int value = calc();
    endfunction
endclass
)";
    const std::string b = R"(class B;
    function int calc();
        return 2;
    endfunction

    function void use();
        int value = calc();
    endfunction
endclass
)";

    {
        std::ofstream out(a_path);
        REQUIRE(out.good());
        out << a;
    }
    {
        std::ofstream out(b_path);
        REQUIRE(out.good());
        out << b;
    }

    Analyzer analyzer;
    analyzer.set_extra_files({a_path.string(), b_path.string()});
    analyzer.wait_for_background_index_idle();

    const std::string a_uri = uri_from_path(a_path);
    analyzer.open(a_uri, a);

    const auto [line, col] = find_position(a, "calc();");
    const auto refs = analyzer.find_references(a_uri, line, col, true);

    REQUIRE(refs.size() == 2);
    for (const auto& ref : refs)
        CHECK(ref.uri == a_uri);
    CHECK(refs[0].line == 1);
    CHECK(refs[0].col == 17);
    CHECK(refs[1].line == 6);
    CHECK(refs[1].col == 20);

    std::filesystem::remove(a_path);
    std::filesystem::remove(b_path);
    std::filesystem::remove(dir);
}

TEST_CASE("references: object method call excludes same-name unit task", "[references]") {
    const auto dir =
        std::filesystem::temp_directory_path() / "lazyverilog_refs_object_method_vs_unit_task";
    std::filesystem::create_directories(dir);

    const auto header_path = dir / "params.svh";
    const auto top_path = dir / "top.sv";

    const std::string header = R"(task req_data();
endtask

class Packet;
    int data;

    task req_data();
    endtask
endclass
)";
    const std::string top = R"(`include "params.svh"
module top;
    always_comb begin
        Packet p;
        p.req_data();
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
    analyzer.set_extra_files({header_path.string()});
    analyzer.wait_for_background_index_idle();

    const std::string top_uri = uri_from_path(top_path);
    const std::string header_uri = uri_from_path(header_path);
    analyzer.open(top_uri, top);

    const auto [line, col] = find_position(top, "req_data();");
    const auto def = analyzer.definition_of(top_uri, line, col);
    REQUIRE(def.has_value());
    CHECK(def->uri == header_uri);
    CHECK(def->line == 6);

    const auto refs = analyzer.find_references(top_uri, line, col, true);
    REQUIRE(refs.size() == 2);
    CHECK(std::any_of(refs.begin(), refs.end(), [&](const Location& ref) {
        return ref.uri == header_uri && ref.line == 6;
    }));
    CHECK(std::any_of(refs.begin(), refs.end(), [&](const Location& ref) {
        return ref.uri == top_uri && ref.line == 4;
    }));
    CHECK(std::none_of(refs.begin(), refs.end(), [&](const Location& ref) {
        return ref.uri == header_uri && ref.line == 0;
    }));

    std::filesystem::remove(header_path);
    std::filesystem::remove(top_path);
    std::filesystem::remove(dir);
}

TEST_CASE("references: changed watched file refreshes only that project shard", "[references]") {
    const auto path =
        std::filesystem::temp_directory_path() / "lazyverilog_refs_watched_refresh.sv";
    {
        std::ofstream out(path);
        REQUIRE(out.good());
        out << "module old_name; endmodule\n";
    }

    Analyzer analyzer;
    analyzer.set_extra_files({path.string()});
    analyzer.wait_for_background_index_idle();

    auto project = analyzer.project_index_snapshot();
    REQUIRE(project);
    REQUIRE(project->module_by_name.size() == 1);
    CHECK(project->module_by_name.contains("old_name"));

    {
        std::ofstream out(path, std::ios::trunc);
        REQUIRE(out.good());
        out << "module new_name; endmodule\n";
    }

    // Simulate the exact event the server receives from workspace edits / file
    // watchers.  The analyzer should enqueue this one file only; no mtime scan
    // or full filelist refresh is needed to replace the stale closed-file shard.
    analyzer.refresh_changed_extra_files({uri_from_path(path)});
    analyzer.wait_for_background_index_idle();

    project = analyzer.project_index_snapshot();
    REQUIRE(project);
    REQUIRE(project->module_by_name.size() == 1);
    CHECK(project->module_by_name.contains("new_name"));

    std::filesystem::remove(path);
}

TEST_CASE("references: renamed package typedef finds closed project uses", "[references]") {
    const auto dir = std::filesystem::temp_directory_path() / "lazyverilog_refs_renamed_pkg_typedef";
    std::filesystem::create_directories(dir);

    const auto header_path = dir / "params.svh";
    const auto use_path = dir / "memory.sv";

    const std::string header = R"(package cpu_pkg;
typedef enum logic [1:0] {
    IDLE,
    FETCH
} states_t;
endpackage
)";
    const std::string use = R"(`include "params.svh"
module memory;
    import cpu_pkg::*;
    states_t state;
    always_comb state = states_t::IDLE;
endmodule
)";

    {
        std::ofstream out(header_path);
        REQUIRE(out.good());
        out << header;
    }
    {
        std::ofstream out(use_path);
        REQUIRE(out.good());
        out << use;
    }

    Analyzer analyzer;
    analyzer.set_include_dirs({dir.string()});
    analyzer.set_extra_files({use_path.string()});
    analyzer.wait_for_background_index_idle();

    const auto header_uri = uri_from_path(header_path);
    analyzer.open(header_uri, header);

    const auto [line, col] = find_position(header, "states_t");
    const auto refs = analyzer.find_references(header_uri, line, col, true);

    REQUIRE(refs.size() == 3);
    CHECK(std::any_of(refs.begin(), refs.end(), [&](const Location& ref) {
        return ref.uri == header_uri && ref.line == 4 && ref.col == 2;
    }));

    std::filesystem::remove(use_path);
    std::filesystem::remove(header_path);
    std::filesystem::remove(dir);
}


TEST_CASE("references: package typedef excludes unimported includer uses", "[references]") {
    const auto dir = std::filesystem::temp_directory_path() / "lazyverilog_refs_unimported_pkg_typedef";
    std::filesystem::create_directories(dir);

    const auto header_path = dir / "params.svh";
    const auto use_path = dir / "memory.sv";

    const std::string header = R"(package cpu_pkg;
typedef enum logic [1:0] {
    IDLE,
    FETCH
} state_t;
endpackage
)";
    const std::string use = R"(`include "params.svh"
module memory;
    state_t state;
    always_comb state = state_t::IDLE;
endmodule
)";

    {
        std::ofstream out(header_path);
        REQUIRE(out.good());
        out << header;
    }
    {
        std::ofstream out(use_path);
        REQUIRE(out.good());
        out << use;
    }

    Analyzer analyzer;
    analyzer.set_include_dirs({dir.string()});
    analyzer.set_extra_files({use_path.string()});
    analyzer.wait_for_background_index_idle();

    const auto header_uri = uri_from_path(header_path);
    const auto use_uri = uri_from_path(use_path);
    analyzer.open(header_uri, header);

    const auto [line, col] = find_position(header, "state_t");
    const auto refs = analyzer.find_references(header_uri, line, col, true);

    REQUIRE(refs.size() == 1);
    CHECK(refs[0].uri == header_uri);
    CHECK(std::none_of(refs.begin(), refs.end(), [&](const Location& ref) {
        return ref.uri == use_uri;
    }));

    std::filesystem::remove(use_path);
    std::filesystem::remove(header_path);
    std::filesystem::remove(dir);
}

TEST_CASE("references: package typedef from included header matches open includer uses",
          "[references]") {
    const auto dir = std::filesystem::temp_directory_path() / "lazyverilog_refs_include_pkg_typedef";
    std::filesystem::create_directories(dir);

    const auto header_path = dir / "params.svh";
    const auto use_path = dir / "memory.sv";

    const std::string header = R"(package cpu_pkg;
typedef enum logic [1:0] {
    IDLE,
    FETCH
} states_t;
endpackage
)";
    const std::string use = R"(`include "params.svh"
module memory;
    import cpu_pkg::*;
    states_t state;
    always_comb state = states_t::IDLE;
endmodule
)";

    {
        std::ofstream out(header_path);
        REQUIRE(out.good());
        out << header;
    }
    {
        std::ofstream out(use_path);
        REQUIRE(out.good());
        out << use;
    }

    Analyzer analyzer;
    analyzer.set_include_dirs({dir.string()});
    const auto header_uri = uri_from_path(header_path);
    const auto use_uri = uri_from_path(use_path);
    analyzer.open(header_uri, header);
    analyzer.open(use_uri, use);

    const auto [line, col] = find_position(header, "states_t");
    const auto refs = analyzer.find_references(header_uri, line, col, true);

    REQUIRE(refs.size() == 3);
    CHECK(std::any_of(refs.begin(), refs.end(), [&](const Location& ref) {
        return ref.uri == header_uri && ref.line == 4 && ref.col == 2;
    }));
    CHECK(std::any_of(refs.begin(), refs.end(), [&](const Location& ref) {
        return ref.uri == use_uri && ref.line == 3 && ref.col == 4;
    }));
    CHECK(std::any_of(refs.begin(), refs.end(), [&](const Location& ref) {
        return ref.uri == use_uri && ref.line == 4 && ref.col == 24;
    }));

    std::filesystem::remove(use_path);
    std::filesystem::remove(header_path);
    std::filesystem::remove(dir);
}

TEST_CASE("references: renamed unsaved included typedef reparses open includer",
          "[references]") {
    const auto dir = std::filesystem::temp_directory_path() / "lazyverilog_refs_unsaved_include_typedef";
    std::filesystem::create_directories(dir);

    const auto header_path = dir / "params.svh";
    const auto use_path = dir / "memory.sv";

    const std::string disk_header = R"(package cpu_pkg;
typedef enum logic [1:0] {
    IDLE,
    FETCH
} state_t;
endpackage
)";
    const std::string open_header = R"(package cpu_pkg;
typedef enum logic [1:0] {
    IDLE,
    FETCH
} states_t;
endpackage
)";
    const std::string disk_use = R"(`include "params.svh"
module memory;
    import cpu_pkg::*;
    state_t state;
    always_comb state = state_t::IDLE;
endmodule
)";
    const std::string open_use = R"(`include "params.svh"
module memory;
    import cpu_pkg::*;
    states_t state;
    always_comb state = states_t::IDLE;
endmodule
)";

    {
        std::ofstream out(header_path);
        REQUIRE(out.good());
        out << disk_header;
    }
    {
        std::ofstream out(use_path);
        REQUIRE(out.good());
        out << disk_use;
    }

    Analyzer analyzer;
    analyzer.set_include_dirs({dir.string()});
    const auto header_uri = uri_from_path(header_path);
    const auto use_uri = uri_from_path(use_path);

    // Match the relevant Neovim rename ordering from /tmp/lsp-cpp.log without
    // any server-side WorkspaceEdit prediction:
    //
    // 1. params.svh is open with the old typedef spelling.
    // 2. Neovim opens memory.sv as part of applying edits, initially with the
    //    old on-disk text.
    // 3. Neovim sends didChange for params.svh.
    // 4. Neovim sends didChange for memory.sv.
    //
    // The final memory.sv parse must see the open, unsaved params.svh overlay;
    // otherwise its visible `states_t` tokens remain unresolved and references
    // from the typedef declaration only return the declaration itself.
    analyzer.open(header_uri, disk_header);
    analyzer.open(use_uri, disk_use);
    analyzer.change(header_uri, open_header);
    analyzer.change(use_uri, open_use);

    const auto [line, col] = find_position(open_header, "states_t");
    const auto refs = analyzer.find_references(header_uri, line, col, true);

    REQUIRE(refs.size() == 3);
    CHECK(std::any_of(refs.begin(), refs.end(), [&](const Location& ref) {
        return ref.uri == header_uri && ref.line == 4 && ref.col == 2;
    }));
    CHECK(std::any_of(refs.begin(), refs.end(), [&](const Location& ref) {
        return ref.uri == use_uri && ref.line == 3 && ref.col == 4;
    }));
    CHECK(std::any_of(refs.begin(), refs.end(), [&](const Location& ref) {
        return ref.uri == use_uri && ref.line == 4 && ref.col == 24;
    }));

    std::filesystem::remove(use_path);
    std::filesystem::remove(header_path);
    std::filesystem::remove(dir);
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
    analyzer.wait_for_background_index_idle();
    const std::string header_uri = uri_from_path(header_path);
    analyzer.open(header_uri, header);

    const auto [line, col] = find_position_after(header, "id;", "[3:0]");
    const auto refs = analyzer.find_references(header_uri, line, col, true);

    REQUIRE(refs.size() == 2);
    CHECK(refs[0].uri == header_uri);
    CHECK(refs[0].line == 2);
    CHECK(refs[0].col == 16);
    CHECK(refs[1].uri == uri_from_path(top_path));
    CHECK(refs[1].line == 4);
    CHECK(refs[1].col == 13);

    std::filesystem::remove(top_path);
    std::filesystem::remove(header_path);
    std::filesystem::remove(dir);
}

TEST_CASE("references: included class method declaration matches closed member call",
          "[references]") {
    const auto dir = std::filesystem::temp_directory_path() /
                     "lazyverilog_refs_include_class_method";
    std::filesystem::create_directories(dir);

    const auto header_path = dir / "params.svh";
    const auto top_path = dir / "memory_top.sv";

    const std::string header = R"(class Packet;
    int data;
    task req_data();
    endtask
endclass
)";
    const std::string top = R"(`include "params.svh"

module memory_top;
    always_comb begin
        Packet p;
        p.req_data();
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
    analyzer.wait_for_background_index_idle();

    const std::string header_uri = uri_from_path(header_path);
    const std::string top_uri = uri_from_path(top_path);
    analyzer.open(header_uri, header);

    const auto [line, col] = find_position(header, "req_data");
    const auto refs = analyzer.find_references(header_uri, line, col, true);

    REQUIRE(refs.size() == 2);
    CHECK(std::any_of(refs.begin(), refs.end(), [&](const Location& ref) {
        return ref.uri == header_uri && ref.line == 2 && ref.col == 9;
    }));
    CHECK(std::any_of(refs.begin(), refs.end(), [&](const Location& ref) {
        return ref.uri == top_uri && ref.line == 5 && ref.col == 10;
    }));

    std::filesystem::remove(header_path);
    std::filesystem::remove(top_path);
    std::filesystem::remove(dir);
}

TEST_CASE("references: package class field declaration matches closed member access",
          "[references]") {
    const auto dir = std::filesystem::temp_directory_path() /
                     "lazyverilog_refs_package_class_field";
    std::filesystem::create_directories(dir);

    const auto pkg_path = dir / "pkt_pkg.sv";
    const auto top_path = dir / "memory_top.sv";

    const std::string pkg = R"(package pkt_pkg;
    class Packet;
        int data;
        task req_data();
        endtask
    endclass
endpackage
)";
    const std::string top = R"(module memory_top;
    import pkt_pkg::*;
    always_comb begin
        Packet p;
        p.data = 1;
        p.req_data();
    end
endmodule
)";

    {
        std::ofstream out(pkg_path);
        REQUIRE(out.good());
        out << pkg;
    }
    {
        std::ofstream out(top_path);
        REQUIRE(out.good());
        out << top;
    }

    Analyzer analyzer;
    analyzer.set_extra_files({pkg_path.string(), top_path.string()});
    analyzer.wait_for_background_index_idle();

    const std::string pkg_uri = uri_from_path(pkg_path);
    const std::string top_uri = uri_from_path(top_path);
    analyzer.open(pkg_uri, pkg);

    const auto [line, col] = find_position(pkg, "data");
    const auto refs = analyzer.find_references(pkg_uri, line, col, true);

    REQUIRE(refs.size() == 2);
    CHECK(std::any_of(refs.begin(), refs.end(), [&](const Location& ref) {
        return ref.uri == pkg_uri && ref.line == 2 && ref.col == 12;
    }));
    CHECK(std::any_of(refs.begin(), refs.end(), [&](const Location& ref) {
        return ref.uri == top_uri && ref.line == 4 && ref.col == 10;
    }));

    std::filesystem::remove(pkg_path);
    std::filesystem::remove(top_path);
    std::filesystem::remove(dir);
}

namespace {

// pkt_base.sv / pkt_child.sv from demo/, the smallest two-file inheritance
// fixture: `pkt_child extends pkt_base` and reaches `depth` without qualifying
// it.  Both files are project files; neither is open unless a test opens it.
struct InheritanceFixture {
    std::filesystem::path dir;
    std::filesystem::path base_path;
    std::filesystem::path child_path;
    std::string base_text;
    std::string child_text;
};

static InheritanceFixture make_inheritance_fixture(const std::string& name) {
    InheritanceFixture fx;
    fx.dir = std::filesystem::temp_directory_path() / name;
    std::filesystem::create_directories(fx.dir);
    fx.base_path = fx.dir / "pkt_base.sv";
    fx.child_path = fx.dir / "pkt_child.sv";

    fx.base_text = R"(class pkt_base;
    int depth;

    function void apply();
    endfunction
endclass
)";
    fx.child_text = R"(class pkt_child extends pkt_base;
    function void bump();
        depth = depth + 1;
        apply();
    endfunction
endclass
)";

    {
        std::ofstream out(fx.base_path);
        REQUIRE(out.good());
        out << fx.base_text;
    }
    {
        std::ofstream out(fx.child_path);
        REQUIRE(out.good());
        out << fx.child_text;
    }
    return fx;
}

static void remove_inheritance_fixture(const InheritanceFixture& fx) {
    std::filesystem::remove(fx.base_path);
    std::filesystem::remove(fx.child_path);
    std::filesystem::remove(fx.dir);
}

} // namespace

TEST_CASE("references: base class declaration finds an extends clause in another file",
          "[references][inheritance]") {
    // `rename` is built on this list, so a miss here silently leaves
    // `class pkt_child extends pkt_base;` pointing at a name that no longer
    // exists.
    const auto fx = make_inheritance_fixture("lazyverilog_refs_extends_clause");

    Analyzer analyzer;
    analyzer.set_extra_files({fx.base_path.string(), fx.child_path.string()});
    analyzer.wait_for_background_index_idle();

    const std::string base_uri = uri_from_path(fx.base_path);
    const std::string child_uri = uri_from_path(fx.child_path);
    analyzer.open(base_uri, fx.base_text);

    const auto [line, col] = find_position(fx.base_text, "pkt_base");
    const auto refs = analyzer.find_references(base_uri, line, col, true);

    CHECK(std::any_of(refs.begin(), refs.end(), [&](const Location& ref) {
        return ref.uri == base_uri && ref.line == 0;
    }));
    CHECK(std::any_of(refs.begin(), refs.end(), [&](const Location& ref) {
        return ref.uri == child_uri && ref.line == 0 &&
               ref.col == (int)fx.child_text.find("pkt_base");
    }));

    remove_inheritance_fixture(fx);
}

TEST_CASE("references: base class field declaration finds inherited uses in another file",
          "[references][inheritance]") {
    // `depth` inside pkt_child is an unqualified inherited field access, which
    // go-to-definition already walks the `extends` chain for.  References has to
    // agree, or rename drops every derived-class use.
    const auto fx = make_inheritance_fixture("lazyverilog_refs_inherited_field");

    Analyzer analyzer;
    analyzer.set_extra_files({fx.base_path.string(), fx.child_path.string()});
    analyzer.wait_for_background_index_idle();

    const std::string base_uri = uri_from_path(fx.base_path);
    const std::string child_uri = uri_from_path(fx.child_path);
    analyzer.open(base_uri, fx.base_text);

    const auto [line, col] = find_position(fx.base_text, "depth");
    const auto refs = analyzer.find_references(base_uri, line, col, true);

    CHECK(std::any_of(refs.begin(), refs.end(), [&](const Location& ref) {
        return ref.uri == base_uri && ref.line == 1;
    }));
    const auto child_uses = std::count_if(refs.begin(), refs.end(), [&](const Location& ref) {
        return ref.uri == child_uri && ref.line == 2;
    });
    CHECK(child_uses == 2); // `depth = depth + 1;`

    remove_inheritance_fixture(fx);
}

TEST_CASE("references: base class method declaration finds inherited calls in another file",
          "[references][inheritance]") {
    const auto fx = make_inheritance_fixture("lazyverilog_refs_inherited_method");

    Analyzer analyzer;
    analyzer.set_extra_files({fx.base_path.string(), fx.child_path.string()});
    analyzer.wait_for_background_index_idle();

    const std::string base_uri = uri_from_path(fx.base_path);
    const std::string child_uri = uri_from_path(fx.child_path);
    analyzer.open(base_uri, fx.base_text);

    const auto [line, col] = find_position(fx.base_text, "apply");
    const auto refs = analyzer.find_references(base_uri, line, col, true);

    CHECK(std::any_of(refs.begin(), refs.end(), [&](const Location& ref) {
        return ref.uri == base_uri && ref.line == 3;
    }));
    CHECK(std::any_of(refs.begin(), refs.end(), [&](const Location& ref) {
        return ref.uri == child_uri && ref.line == 3;
    }));

    remove_inheritance_fixture(fx);
}

TEST_CASE("references: method-local stays scoped when its own file is indexed",
          "[references]") {
    const auto dir = std::filesystem::temp_directory_path() / "lazyverilog_refs_method_local";
    std::filesystem::create_directories(dir);

    const auto drv_path = dir / "drv.sv";
    const auto lib_path = dir / "lib.sv";

    const std::string drv = R"(class my_driver;
    task run_phase();
        int item;
        item = 1;
    endtask
endclass
)";
    const std::string lib = R"(module lib;
    initial begin
        int item;
        item = 2;
    end
endmodule
)";

    {
        std::ofstream out(drv_path);
        REQUIRE(out.good());
        out << drv;
    }
    {
        std::ofstream out(lib_path);
        REQUIRE(out.good());
        out << lib;
    }

    Analyzer analyzer;
    // The open file is in the filelist too, so it has a closed shard of its own
    // that offers a weak `name:item` identity for this method-local.
    analyzer.set_extra_files({drv_path.string(), lib_path.string()});
    analyzer.wait_for_background_index_idle();

    const std::string drv_uri = uri_from_path(drv_path);
    analyzer.open(drv_uri, drv);

    const auto [line, col] = find_position(drv, "item;");
    const auto refs = analyzer.find_references(drv_uri, line, col, true);

    CHECK(refs.size() == 2);
    for (const auto& ref : refs)
        CHECK(ref.uri == drv_uri);

    std::filesystem::remove(drv_path);
    std::filesystem::remove(lib_path);
    std::filesystem::remove(dir);
}

TEST_CASE("references: package class method declaration matches closed member call",
          "[references]") {
    const auto dir = std::filesystem::temp_directory_path() /
                     "lazyverilog_refs_package_class_method";
    std::filesystem::create_directories(dir);

    const auto pkg_path = dir / "pkt_pkg.sv";
    const auto top_path = dir / "memory_top.sv";

    const std::string pkg = R"(package pkt_pkg;
    class Packet;
        int data;
        task req_data();
        endtask
    endclass
endpackage
)";
    const std::string top = R"(module memory_top;
    import pkt_pkg::*;
    always_comb begin
        Packet p;
        p.data = 1;
        p.req_data();
    end
endmodule
)";

    {
        std::ofstream out(pkg_path);
        REQUIRE(out.good());
        out << pkg;
    }
    {
        std::ofstream out(top_path);
        REQUIRE(out.good());
        out << top;
    }

    Analyzer analyzer;
    analyzer.set_extra_files({pkg_path.string(), top_path.string()});
    analyzer.wait_for_background_index_idle();

    const std::string pkg_uri = uri_from_path(pkg_path);
    const std::string top_uri = uri_from_path(top_path);
    analyzer.open(pkg_uri, pkg);

    const auto [line, col] = find_position(pkg, "req_data");
    const auto refs = analyzer.find_references(pkg_uri, line, col, true);

    REQUIRE(refs.size() == 2);
    CHECK(std::any_of(refs.begin(), refs.end(), [&](const Location& ref) {
        return ref.uri == pkg_uri && ref.line == 3;
    }));
    CHECK(std::any_of(refs.begin(), refs.end(), [&](const Location& ref) {
        return ref.uri == top_uri && ref.line == 5 && ref.col == 10;
    }));

    std::filesystem::remove(pkg_path);
    std::filesystem::remove(top_path);
    std::filesystem::remove(dir);
}

TEST_CASE("references: included header declarations bridge unresolved includer names",
          "[references]") {
    const auto dir = std::filesystem::temp_directory_path() /
                     "lazyverilog_refs_include_symbol_bridge";
    std::filesystem::create_directories(dir);

    const auto header_path = dir / "params.svh";
    const auto top_path = dir / "memory_top.sv";

    const std::string header = R"(typedef enum logic [1:0] {
    IDLE,
    RUN
} state_t;

typedef struct packed {
    logic valid;
} packet_t;

class Packet;
    int data;
endclass

parameter int DATA_WIDTH = 32;
logic [3:0] global_arr;
)";
    const std::string top = R"(`include "params.svh"

module memory_top;
    state_t state;
    packet_t pkt;
    Packet p;
    logic [DATA_WIDTH-1:0] bus;

    initial begin
        state = RUN;
        bus = global_arr[0];
        bus = pkt.valid;
        p.data = bus;
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
    analyzer.wait_for_background_index_idle();

    const std::string header_uri = uri_from_path(header_path);
    const std::string top_uri = uri_from_path(top_path);
    analyzer.open(header_uri, header);

    auto has_top_reference = [&](std::string_view declaration_needle,
                                 std::string_view use_needle) {
        const auto [decl_line, decl_col] = find_position(header, declaration_needle);
        const auto [use_line, use_col] = find_position(top, use_needle);
        const auto refs = analyzer.find_references(header_uri, decl_line, decl_col, false);
        return std::any_of(refs.begin(), refs.end(), [&](const Location& ref) {
            return ref.uri == top_uri && ref.line == use_line && ref.col == use_col;
        });
    };

    // Each declaration lives in the standalone opened header, while each use is
    // in a closed project shard that only sees the declarations through
    // `include.  If the includer cannot classify a token precisely, it records
    // `name:<identifier>`; find-references must bridge those unresolved names
    // only for shards that actually include the declaration source file.
    CHECK(has_top_reference("state_t;", "state_t state"));
    CHECK(has_top_reference("RUN", "RUN"));
    CHECK(has_top_reference("packet_t;", "packet_t pkt"));
    CHECK(has_top_reference("valid;", "valid"));
    CHECK(has_top_reference("Packet;", "Packet p"));
    CHECK(has_top_reference("data;", "data ="));
    CHECK(has_top_reference("DATA_WIDTH", "DATA_WIDTH"));
    CHECK(has_top_reference("global_arr", "global_arr"));

    std::filesystem::remove(header_path);
    std::filesystem::remove(top_path);
    std::filesystem::remove(dir);
}

TEST_CASE("references: included subroutine declaration matches closed includer call",
          "[references]") {
    const auto dir = std::filesystem::temp_directory_path() /
                     "lazyverilog_refs_include_subroutine_decl";
    std::filesystem::create_directories(dir);

    const auto header_path = dir / "params.svh";
    const auto top_path = dir / "memory_top.sv";

    const std::string header = R"(task add_number();
endtask
)";
    const std::string top = R"(`include "params.svh"

module memory_top;
    initial begin
        add_number();
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
    analyzer.wait_for_background_index_idle();

    const std::string header_uri = uri_from_path(header_path);
    analyzer.open(header_uri, header);

    const auto [line, col] = find_position(header, "add_number");
    const auto refs = analyzer.find_references(header_uri, line, col, true);

    const std::string top_uri = uri_from_path(top_path);
    REQUIRE(refs.size() == 2);
    CHECK(std::any_of(refs.begin(), refs.end(), [&](const Location& ref) {
        return ref.uri == header_uri && ref.line == 0 && ref.col == 5;
    }));
    CHECK(std::any_of(refs.begin(), refs.end(), [&](const Location& ref) {
        return ref.uri == top_uri && ref.line == 4 && ref.col == 8;
    }));

    std::filesystem::remove(header_path);
    std::filesystem::remove(top_path);
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
    analyzer.wait_for_background_index_idle();

    const auto snapshots = analyzer.extra_index_snapshot_ptr();
    REQUIRE(snapshots != nullptr);
    // The including file's shard plus the header's own shard.  Header
    // occurrences are indexed once, under the header, so look across shards
    // rather than inside the includer's.
    REQUIRE(snapshots->size() == 2);
    const bool indexed_add_number = std::any_of(
        snapshots->begin(), snapshots->end(), [&](const ExtraIndexInfo& info) {
            const auto& refs = info.index_ref().references;
            return std::any_of(refs.begin(), refs.end(), [&](const ReferenceEntry& ref) {
                return ref.name == "add_number" &&
                       info.index_ref().source_uri(ref.file_id) == uri_from_path(header_path);
            });
        });
    REQUIRE(indexed_add_number);

    const std::string top_uri = uri_from_path(top_path);
    const std::string closed_uri = uri_from_path(closed_path);
    analyzer.open(top_uri, top);

    const auto [line, col] = find_position(top, "add_number();");
    const auto refs = analyzer.find_references(top_uri, line, col, true);

    const std::string header_uri = uri_from_path(header_path);
    REQUIRE(refs.size() == 2);
    CHECK(std::any_of(refs.begin(), refs.end(), [&](const Location& ref) {
        return ref.uri == header_uri && ref.line == 0 && ref.col == 5;
    }));
    CHECK(std::any_of(refs.begin(), refs.end(), [&](const Location& ref) {
        return ref.uri == top_uri && ref.line == 2 && ref.col == 12;
    }));
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
    analyzer.wait_for_background_index_idle();

    const std::string header_uri = uri_from_path(header_path);
    const std::string includer_uri = uri_from_path(includer_path);
    analyzer.open(header_uri, header);

    const auto snapshots = analyzer.extra_index_snapshot_ptr();
    REQUIRE(snapshots != nullptr);
    REQUIRE(snapshots->size() == 2);
    const bool indexed_state_t = std::any_of(
        snapshots->begin(), snapshots->end(), [&](const ExtraIndexInfo& info) {
            const auto& refs = info.index_ref().references;
            return std::any_of(refs.begin(), refs.end(), [&](const ReferenceEntry& ref) {
                return ref.name == "state_t" &&
                       info.index_ref().source_uri(ref.file_id) == header_uri &&
                       ref.symbol_debug == "typedef::state_t";
            });
        });
    REQUIRE(indexed_state_t);

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

TEST_CASE("references: package parameter declaration finds qualified uses", "[references]") {
    const std::string text = R"(package p1;
    parameter int WIDTH = 8;
endpackage

module top;
    logic [p1::WIDTH-1:0] data;
    logic [p1::WIDTH-1:0] more;
endmodule
)";

    Analyzer analyzer;
    const std::string uri = "file:///tmp/lazyverilog_refs_pkg_param.sv";
    analyzer.open(uri, text);

    // Invoke from the declaration itself.
    auto [line, col] = find_position_after(text, "WIDTH", "parameter int ");
    auto refs = analyzer.find_references(uri, line, col, true);

    // Declaration plus both qualified uses.
    REQUIRE(refs.size() == 3);
    for (const auto& ref : refs)
        CHECK(ref.uri == uri);
    CHECK(refs[0].line == 1);
    CHECK(refs[1].line == 5);
    CHECK(refs[2].line == 6);
}

TEST_CASE("references: module signal excludes same-named included struct field",
          "[references]") {
    const auto dir =
        std::filesystem::temp_directory_path() / "lazyverilog_refs_include_struct_field_shadow";
    std::filesystem::create_directories(dir);

    const auto header_path = dir / "params.svh";
    const auto top_path = dir / "top.sv";

    const std::string header = R"(typedef struct packed {
    state_t state;
} a_t;

typedef enum logic [1:0] {
    IDLE,
    FETCH
} state_t;
)";
    const std::string top = R"(module top;
`include "params.svh"
state_t state;
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
    analyzer.set_extra_files({header_path.string()});
    analyzer.wait_for_background_index_idle();

    const std::string top_uri = uri_from_path(top_path);
    const std::string header_uri = uri_from_path(header_path);
    analyzer.open(top_uri, top);

    const auto [line, col] = find_position_after(top, "state", "state_t ");
    const auto refs = analyzer.find_references(top_uri, line, col, true);

    CHECK(std::none_of(refs.begin(), refs.end(),
                       [&](const Location& ref) { return ref.uri == header_uri; }));
    REQUIRE(refs.size() == 1);
    CHECK(refs[0].uri == top_uri);
    CHECK(refs[0].line == 2);

    std::filesystem::remove(top_path);
    std::filesystem::remove(header_path);
    std::filesystem::remove(dir);
}

TEST_CASE("references: package members are found in files that import them", "[references]") {
    const auto pkg = write_temp_sv("lazyverilog_refs_import_pkg.sv",
                                   "package common_pkg;\n"
                                   "typedef int my_int_t;\n"
                                   "typedef enum int { RED, GREEN } color_e;\n"
                                   "localparam int WIDTH = 8;\n"
                                   "function void foo();\n"
                                   "endfunction\n"
                                   "class my_cls;\n"
                                   "endclass\n"
                                   "endpackage\n");
    const auto use = write_temp_sv("lazyverilog_refs_import_use.sv",
                                   "module top;\n"
                                   "import common_pkg::*;\n"
                                   "    my_int_t x;\n"
                                   "    color_e c = RED;\n"
                                   "    logic [WIDTH-1:0] bus;\n"
                                   "    my_cls handle;\n"
                                   "    initial begin\n"
                                   "        foo();\n"
                                   "    end\n"
                                   "endmodule\n");
    const std::string pkg_uri = uri_from_path(pkg);
    const std::string use_uri = uri_from_path(use);
    auto read = [](const std::filesystem::path& path) {
        std::ifstream in(path);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    };

    // Every package member kind must reach its use site, whether the importing
    // file is an open buffer or only a closed project shard.
    auto uses_in_importer = [&](int line, int col, bool open_importer) {
        Analyzer analyzer;
        analyzer.set_extra_files({pkg.string(), use.string()});
        analyzer.wait_for_background_index_idle();
        analyzer.open(pkg_uri, read(pkg));
        if (open_importer)
            analyzer.open(use_uri, read(use));
        int found = 0;
        for (const auto& ref : analyzer.find_references(pkg_uri, line, col, true)) {
            if (ref.uri == use_uri)
                ++found;
        }
        return found;
    };

    for (bool open_importer : {false, true}) {
        CHECK(uses_in_importer(1, 12, open_importer) == 1);  // typedef my_int_t
        CHECK(uses_in_importer(2, 21, open_importer) == 1);  // enum member RED
        CHECK(uses_in_importer(3, 18, open_importer) == 1);  // localparam WIDTH
        CHECK(uses_in_importer(4, 14, open_importer) == 1);  // function foo
        CHECK(uses_in_importer(6, 6, open_importer) == 1);   // class my_cls
    }

    // Starting from the use site must find the same pair.  The declaration
    // lives in a closed shard here, so this exercises the index-only recovery.
    {
        Analyzer analyzer;
        analyzer.set_extra_files({pkg.string(), use.string()});
        analyzer.wait_for_background_index_idle();
        analyzer.open(use_uri, read(use));
        const auto refs = analyzer.find_references(use_uri, 7, 9, true);
        CHECK(refs.size() == 2);
        CHECK(std::any_of(refs.begin(), refs.end(),
                          [&](const Location& r) { return r.uri == pkg_uri && r.line == 4; }));
    }

    // An unrelated file that does not import the package must not be rewritten.
    {
        const auto other = write_temp_sv("lazyverilog_refs_import_other.sv", "module other;\n"
                                                                            "    int foo;\n"
                                                                            "    initial foo = 1;\n"
                                                                            "endmodule\n");
        Analyzer analyzer;
        analyzer.set_extra_files({pkg.string(), use.string(), other.string()});
        analyzer.wait_for_background_index_idle();
        analyzer.open(pkg_uri, read(pkg));
        for (const auto& ref : analyzer.find_references(pkg_uri, 4, 14, true))
            CHECK(ref.uri != uri_from_path(other));
        std::filesystem::remove(other);
    }

    std::filesystem::remove(pkg);
    std::filesystem::remove(use);
}

TEST_CASE("references: identifiers inside macro arguments are occurrences", "[references][macro]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/lazyverilog_refs_macro_arg.sv";
    analyzer.open(uri, "`define LOG(MSG, LVL) $display(MSG)\n"
                       "`define ID(X) X\n"
                       "module top;\n"
                       "    int item;\n"
                       "    initial begin\n"
                       "        `LOG($sformatf(\"%0d\", item), 1)\n"
                       "        $display(item);\n"
                       "        item = `ID(item);\n"
                       "    end\n"
                       "endmodule\n");

    auto has = [](const std::vector<Location>& refs, int line, int col) {
        return std::any_of(refs.begin(), refs.end(),
                           [&](const Location& r) { return r.line == line && r.col == col; });
    };

    // Rename has to rewrite the names the user typed as macro arguments too;
    // skipping them leaves the call sites referring to a name that is gone.
    for (auto [line, col] : {std::pair{3, 8}, std::pair{5, 30}}) {
        const auto refs = analyzer.find_references(uri, line, col, true);
        CHECK(refs.size() == 5);
        CHECK(has(refs, 3, 8));   // declaration
        CHECK(has(refs, 5, 30));  // nested inside `LOG's argument
        CHECK(has(refs, 6, 17));  // ordinary use
        CHECK(has(refs, 7, 8));   // ordinary use
        CHECK(has(refs, 7, 19));  // `ID's argument, the macro's whole expansion
    }

    // The invocation itself is still a reference to the macro, recorded once at
    // the name the user wrote rather than at the argument's expansion.
    const auto log_refs = analyzer.find_references(uri, 5, 10, true);
    CHECK(log_refs.size() == 2);
    CHECK(has(log_refs, 0, 8));
    CHECK(has(log_refs, 5, 9));

    const auto id_refs = analyzer.find_references(uri, 7, 16, true);
    CHECK(id_refs.size() == 2);
    CHECK(has(id_refs, 1, 8));
    CHECK(has(id_refs, 7, 16));
}

TEST_CASE("rename: a macro body identifier does not answer for the whole invocation",
          "[references][rename][macro]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/lazyverilog_rename_macro_body.sv";
    analyzer.open(uri, "`define uvm_info(ID, MSG, VERB) uvm_report_info(ID, MSG, VERB)\n"
                       "module top;\n"
                       "    int item;\n"
                       "    initial begin\n"
                       "        `uvm_info(\"DRV\", $sformatf(\"%0d\", item), UVM_MEDIUM)\n"
                       "    end\n"
                       "endmodule\n");

    const std::string call = "        `uvm_info(\"DRV\", $sformatf(\"%0d\", item), UVM_MEDIUM)";

    // The macro expands to a call to uvm_report_info.  That name is written in
    // the `define, not here, so it must not be what rename offers at a column
    // the user typed something else at.
    const int item_col = (int)call.find("item");
    auto on_arg = analyzer.identifier_at(uri, 4, item_col);
    REQUIRE(on_arg.has_value());
    CHECK(on_arg->name == "item");
    CHECK(on_arg->line == 4);
    CHECK(on_arg->col == item_col);

    // The invocation itself still renames as the macro.
    const int macro_col = (int)call.find("uvm_info");
    auto on_macro = analyzer.identifier_at(uri, 4, macro_col);
    REQUIRE(on_macro.has_value());
    CHECK(on_macro->name == "uvm_info");
    CHECK(on_macro->line == 4);
    CHECK(on_macro->col == macro_col);

    const auto macro_refs = analyzer.find_references(uri, 4, macro_col, true);
    CHECK(macro_refs.size() == 2);
    CHECK(std::any_of(macro_refs.begin(), macro_refs.end(),
                      [](const Location& r) { return r.line == 0 && r.col == 8; }));
    CHECK(std::any_of(macro_refs.begin(), macro_refs.end(), [&](const Location& r) {
        return r.line == 4 && r.col == macro_col;
    }));
}

TEST_CASE("references: a method-local reaches its uses inside macro arguments",
          "[references][macro]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/lazyverilog_refs_method_local_macro.sv";
    analyzer.open(uri, "`define INFO(A) uvm_report_info(A)\n"
                       "class item_t;\n"
                       "    bit [7:0] addr;\n"
                       "endclass\n"
                       "class drv;\n"
                       "    task run();\n"
                       "        item_t item;\n"
                       "        get(item);\n"
                       "        `INFO(item)\n"
                       "        `INFO(item.addr)\n"
                       "    endtask\n"
                       "endclass\n");

    // A local declared inside a class method has no scope-qualified SymbolID,
    // so references come from the AST verification path rather than the index.
    // That path still has to place a macro argument where the user wrote it.
    const auto refs = analyzer.find_references(uri, 6, 15, true);
    CHECK(refs.size() == 4);
    auto has = [&](int line, int col) {
        return std::any_of(refs.begin(), refs.end(),
                           [&](const Location& r) { return r.line == line && r.col == col; });
    };
    CHECK(has(6, 15));  // declaration
    CHECK(has(7, 12));  // ordinary use
    CHECK(has(8, 14));  // macro argument
    CHECK(has(9, 14));  // macro argument, object of a member access
}

TEST_CASE("references: an unresolved local name does not match other project files",
          "[references][rename]") {
    // The open file is listed in the filelist, as any real project file is, so
    // it also has a closed shard of its own.  That shard knows the local only as
    // `name:item`, which must not become a bridge to every unrelated `item`.
    const auto lib = write_temp_sv("lazyverilog_refs_local_lib.sv", "class other;\n"
                                                                   "    task run();\n"
                                                                   "        int item;\n"
                                                                   "        use(item);\n"
                                                                   "    endtask\n"
                                                                   "endclass\n");
    const std::string drv_text = "class drv;\n"
                                 "    task run();\n"
                                 "        int item;\n"
                                 "        get(item);\n"
                                 "    endtask\n"
                                 "endclass\n";
    const auto drv = write_temp_sv("lazyverilog_refs_local_drv.sv", drv_text);
    Analyzer analyzer;
    analyzer.set_extra_files({lib.string(), drv.string()});
    analyzer.wait_for_background_index_idle();

    const std::string drv_uri = uri_from_path(drv);
    analyzer.open(drv_uri, drv_text);

    const auto refs = analyzer.find_references(drv_uri, 2, 12, true);
    CHECK(refs.size() == 2);
    for (const auto& ref : refs)
        CHECK(ref.uri == drv_uri);

    std::filesystem::remove(lib);
    std::filesystem::remove(drv);
}

TEST_CASE("references: a class field reaches its handle.field uses", "[references][class]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/lazyverilog_refs_member_access.sv";
    analyzer.open(uri, "class item;\n"
                       "    int addr;\n"
                       "endclass\n"
                       "module top;\n"
                       "    item it;\n"
                       "    initial begin\n"
                       "        it.addr = 1;\n"
                       "        $display(it.addr);\n"
                       "    end\n"
                       "endmodule\n");

    // On the declaration of `addr`.
    auto refs = analyzer.find_references(uri, 1, 8, true);
    CHECK(refs.size() == 3);

    std::vector<int> lines;
    for (const auto& ref : refs)
        lines.push_back(ref.line);
    std::sort(lines.begin(), lines.end());
    CHECK(lines == std::vector<int>{1, 6, 7});
}

TEST_CASE("references: an open file reachable through a symlinked path is not "
          "double-counted against its own closed shard",
          "[references]") {
    // A client can open a document through a symlinked directory while the
    // project filelist reaches the identical on-disk file through its
    // canonical path.  find_references()'s "skip the open file's own closed
    // shard" guard used to compare those two URI spellings as raw strings, so
    // it failed to recognize them as the same file and walked the shard
    // anyway -- doubling every result under a second, differently-spelled URI.
    namespace fs = std::filesystem;
    const auto real_dir = fs::temp_directory_path() / "lazyverilog_refs_symlink_real";
    const auto link_dir = fs::temp_directory_path() / "lazyverilog_refs_symlink_link";
    fs::remove(link_dir);
    fs::remove_all(real_dir);
    fs::create_directories(real_dir);
    std::error_code ec;
    fs::create_directory_symlink(real_dir, link_dir, ec);
    if (ec) {
        SUCCEED("symlinks unavailable on this platform");
        return;
    }

    const auto real_path = real_dir / "item.sv";
    {
        std::ofstream out(real_path);
        REQUIRE(out.good());
        out << "class item;\n"
               "    int addr;\n"
               "endclass\n"
               "module top;\n"
               "    item it;\n"
               "    initial begin\n"
               "        it.addr = 1;\n"
               "        $display(it.addr);\n"
               "    end\n"
               "endmodule\n";
    }

    Analyzer analyzer;
    analyzer.set_extra_files({real_path.string()});
    analyzer.wait_for_background_index_idle();

    // Open through the uncanonicalized (symlinked) spelling on purpose --
    // uri_from_path() would canonicalize it away and hide the mismatch.
    const std::string uri = "file://" + (link_dir / "item.sv").string();
    analyzer.open(uri, "class item;\n"
                       "    int addr;\n"
                       "endclass\n"
                       "module top;\n"
                       "    item it;\n"
                       "    initial begin\n"
                       "        it.addr = 1;\n"
                       "        $display(it.addr);\n"
                       "    end\n"
                       "endmodule\n");

    auto refs = analyzer.find_references(uri, 1, 8, true);
    CHECK(refs.size() == 3);

    fs::remove(link_dir);
    fs::remove_all(real_dir);
}

TEST_CASE("references: package member is found through pkg::name qualification",
          "[references]") {
    // A file that qualifies states the owning package at the use site, so it
    // must be found whether or not it also imports the package.  Before this
    // was indexed, only importing files matched and renaming a package
    // parameter silently left every qualifying file behind.
    const std::string pkg =
        "package cfg_pkg;\n"
        "    parameter int TL_AW = 32;\n"
        "endpackage\n";
    const std::string importer =
        "import cfg_pkg::*;\n"
        "module importer;\n"
        "    logic [TL_AW-1:0] addr;\n"
        "endmodule\n";
    const std::string qualifier_a =
        "module qualifier_a;\n"
        "    logic [cfg_pkg::TL_AW-1:0] addr;\n"
        "endmodule\n";
    const std::string qualifier_b =
        "module qualifier_b;\n"
        "    localparam int W = cfg_pkg::TL_AW;\n"
        "endmodule\n";

    const auto pkg_path = write_temp_sv("lazyverilog_refs_scoped_pkg.sv", pkg);
    const auto importer_path = write_temp_sv("lazyverilog_refs_scoped_importer.sv", importer);
    const auto qa_path = write_temp_sv("lazyverilog_refs_scoped_qa.sv", qualifier_a);
    const auto qb_path = write_temp_sv("lazyverilog_refs_scoped_qb.sv", qualifier_b);

    Analyzer analyzer;
    analyzer.set_extra_files({pkg_path.string(), importer_path.string(), qa_path.string(),
                              qb_path.string()});
    analyzer.wait_for_background_index_idle();

    const std::string pkg_uri = uri_from_path(pkg_path);
    analyzer.open(pkg_uri, pkg);

    const auto [line, col] = find_position(pkg, "TL_AW");
    const auto refs = analyzer.find_references(pkg_uri, line, col, true);

    std::set<std::string> uris;
    for (const auto& ref : refs)
        uris.insert(ref.uri);

    CHECK(uris.count(pkg_uri) == 1);
    CHECK(uris.count(uri_from_path(importer_path)) == 1);
    CHECK(uris.count(uri_from_path(qa_path)) == 1);
    CHECK(uris.count(uri_from_path(qb_path)) == 1);
    CHECK(refs.size() == 4);

    std::filesystem::remove(pkg_path);
    std::filesystem::remove(importer_path);
    std::filesystem::remove(qa_path);
    std::filesystem::remove(qb_path);
}

TEST_CASE("references: pkg::name search from a use site includes its own occurrence",
          "[references]") {
    const std::string pkg =
        "package cfg_pkg;\n"
        "    parameter int TL_AW = 32;\n"
        "endpackage\n";
    const std::string user =
        "module user;\n"
        "    logic [cfg_pkg::TL_AW-1:0] addr;\n"
        "endmodule\n";

    const auto pkg_path = write_temp_sv("lazyverilog_refs_scoped_self_pkg.sv", pkg);
    const auto user_path = write_temp_sv("lazyverilog_refs_scoped_self_user.sv", user);

    Analyzer analyzer;
    analyzer.set_extra_files({pkg_path.string(), user_path.string()});
    analyzer.wait_for_background_index_idle();

    const std::string user_uri = uri_from_path(user_path);
    analyzer.open(user_uri, user);

    const auto [line, col] = find_position(user, "cfg_pkg::TL_AW");
    const auto refs = analyzer.find_references(user_uri, line + 0, col + 9, true);

    bool has_self = false;
    for (const auto& ref : refs)
        if (ref.uri == user_uri && ref.line == 1)
            has_self = true;
    CHECK(has_self);
    CHECK(refs.size() == 2);

    std::filesystem::remove(pkg_path);
    std::filesystem::remove(user_path);
}

TEST_CASE("references: pkg::name does not merge same-named members of other packages",
          "[references]") {
    const std::string pkg_a =
        "package pkg_a;\n"
        "    parameter int WIDTH = 8;\n"
        "endpackage\n";
    const std::string pkg_b =
        "package pkg_b;\n"
        "    parameter int WIDTH = 16;\n"
        "endpackage\n";
    const std::string user =
        "module user;\n"
        "    logic [pkg_a::WIDTH-1:0] a;\n"
        "    logic [pkg_b::WIDTH-1:0] b;\n"
        "endmodule\n";

    const auto a_path = write_temp_sv("lazyverilog_refs_scoped_pkg_a.sv", pkg_a);
    const auto b_path = write_temp_sv("lazyverilog_refs_scoped_pkg_b.sv", pkg_b);
    const auto user_path = write_temp_sv("lazyverilog_refs_scoped_two_pkg_user.sv", user);

    Analyzer analyzer;
    analyzer.set_extra_files({a_path.string(), b_path.string(), user_path.string()});
    analyzer.wait_for_background_index_idle();

    const std::string a_uri = uri_from_path(a_path);
    analyzer.open(a_uri, pkg_a);

    const auto [line, col] = find_position(pkg_a, "WIDTH");
    const auto refs = analyzer.find_references(a_uri, line, col, true);

    // pkg_a::WIDTH and its declaration only -- pkg_b::WIDTH is a different
    // symbol that happens to share a name.
    CHECK(refs.size() == 2);
    for (const auto& ref : refs)
        CHECK(ref.uri != uri_from_path(b_path));

    std::filesystem::remove(a_path);
    std::filesystem::remove(b_path);
    std::filesystem::remove(user_path);
}

TEST_CASE("rename: package member rewrites pkg::name qualified uses", "[references][rename]") {
    const std::string pkg =
        "package cfg_pkg;\n"
        "    parameter int TL_AW = 32;\n"
        "endpackage\n";
    const std::string user =
        "module user;\n"
        "    logic [cfg_pkg::TL_AW-1:0] addr;\n"
        "endmodule\n";

    const auto pkg_path = write_temp_sv("lazyverilog_rename_scoped_pkg.sv", pkg);
    const auto user_path = write_temp_sv("lazyverilog_rename_scoped_user.sv", user);

    Analyzer analyzer;
    analyzer.set_extra_files({pkg_path.string(), user_path.string()});
    analyzer.wait_for_background_index_idle();

    const std::string pkg_uri = uri_from_path(pkg_path);
    analyzer.open(pkg_uri, pkg);

    const auto [line, col] = find_position(pkg, "TL_AW");
    const auto refs = analyzer.find_references(pkg_uri, line, col, true);

    // Rename edits are the reference set, so a qualified use left out here is a
    // file the rename would silently break.
    bool touches_user = false;
    for (const auto& ref : refs)
        if (ref.uri == uri_from_path(user_path))
            touches_user = true;
    CHECK(touches_user);

    std::filesystem::remove(pkg_path);
    std::filesystem::remove(user_path);
}

// References from the module-level declaration used to list the shadowing
// generate-block declaration and its uses, and rename inherited that: renaming
// the outer signal rewrote a different signal inside the generate block.
TEST_CASE("references: a generate-block declaration is not reported for the outer signal",
          "[references]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/references_generate_shadow.sv";
    const std::string text = R"(module top;
  logic [7:0] dout;
  assign dout = 8'h0;

  genvar i;
  generate
    for (i = 0; i < 2; i++) begin : g_lanes
      logic [7:0] dout;
      assign dout = 8'h1;
    end
  endgenerate
endmodule
)";
    analyzer.open(uri, text);

    // From the module-level declaration (line 1, 0-based).
    auto refs = analyzer.find_references(uri, 1, 14, true);
    for (const auto& ref : refs) {
        CAPTURE(ref.line);
        CHECK(ref.line < 7);
    }
    CHECK(refs.size() == 2);
}

// Asking "who calls this?" from a method's declaration used to answer nothing,
// while asking from the out-of-body definition or from a call site worked.  The
// extern prototype is the declaration a UVM class body actually shows, so a
// rename started there silently did nothing.

// Asking "who calls this?" from a method's declaration used to answer nothing,
// while asking from the out-of-body definition or from a call site worked.  The
// extern prototype is the declaration a UVM class body actually shows, so a
// rename started there silently did nothing.
TEST_CASE("references: from an extern method prototype", "[references]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/references_extern_proto.sv";
    analyzer.open(uri, R"(package p;
  class base_c;
    int unsigned tag;
    extern function void bump(int amount);
  endclass

  function void base_c::bump(int amount);
    tag += amount;
  endfunction
endpackage

module top;
  initial begin
    p::base_c obj = new();
    obj.bump(3);
  end
endmodule
)");

    // Cursor on the prototype's name (line 3, 0-based).
    auto refs = analyzer.find_references(uri, 3, 25, true);
    std::vector<int> lines;
    for (const auto& ref : refs)
        lines.push_back(ref.line);
    std::sort(lines.begin(), lines.end());

    CAPTURE(lines);
    CHECK(std::find(lines.begin(), lines.end(), 3) != lines.end());  // prototype
    CHECK(std::find(lines.begin(), lines.end(), 6) != lines.end());  // out-of-body body
    CHECK(std::find(lines.begin(), lines.end(), 14) != lines.end()); // call site
}

// A method declaration knows its callers only if the call sites carry the same
// identity.  `handle.method()` on a package class and `super.method()` were both
// left unresolved, so references started from the declaration returned only the
// declaration itself.

// A method declaration knows its callers only if the call sites carry the same
// identity.  `handle.method()` on a package class and `super.method()` were both
// left unresolved, so references started from the declaration returned only the
// declaration itself.
TEST_CASE("references: from a class method declaration reaches handle and super calls",
          "[references]") {
    Analyzer analyzer;
    const std::string uri = "file:///tmp/references_method_decl.sv";
    analyzer.open(uri, R"(package p;
  class base_c;
    virtual function string name();
      return "base";
    endfunction
  endclass
  class mid_c extends base_c;
  endclass
  class leaf_c extends mid_c;
    virtual function string name();
      string s = super.name();
      return s;
    endfunction
  endclass
endpackage

module top;
  initial begin
    p::leaf_c leaf = new();
    string nm;
    nm = leaf.name();
  end
endmodule
)");

    auto contains_line = [](const std::vector<Location>& refs, int line) {
        return std::any_of(refs.begin(), refs.end(),
                           [&](const Location& loc) { return loc.line == line; });
    };

    // From the base class declaration: the override and the `super.name()` call.
    auto base_refs = analyzer.find_references(uri, 2, 28, true);
    CHECK(contains_line(base_refs, 2));
    CHECK(contains_line(base_refs, 10));

    // From the derived declaration: the `leaf.name()` call site.
    auto leaf_refs = analyzer.find_references(uri, 9, 28, true);
    CHECK(contains_line(leaf_refs, 9));
    CHECK(contains_line(leaf_refs, 20));
}

// `import p::*;` lets a file spell a package type without the `p::` prefix.
// Those unqualified uses were invisible to references and rename, so renaming
// the type left them pointing at a name that no longer exists.
