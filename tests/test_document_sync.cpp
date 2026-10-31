#include <catch2/catch_test_macros.hpp>
#include "analyzer.hpp"
#include "string_utils.hpp"
#include <slang/syntax/AllSyntax.h>
#include <slang/syntax/SyntaxTree.h>
#include <algorithm>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>

static const std::string kUri = "file:///test.sv";
static const std::string kSrc1 = "module foo; endmodule\n";
static const std::string kSrc2 = "module bar; endmodule\n";

TEST_CASE("doc sync: open creates DocumentState with parsed SyntaxTree", "[sync]") {
    Analyzer a;
    a.open(kUri, kSrc1);
    auto state = a.get_state(kUri);
    REQUIRE(state != nullptr);
    CHECK(state->uri == kUri);
    CHECK(state->text == kSrc1);
    CHECK(state->tree != nullptr);
}

TEST_CASE("doc sync: a stringifying macro whose arguments run to end of file parses",
          "[sync]") {
    // slang v10.0 stringified through the current lexer, and by the time an
    // unterminated `S(` is expanded the last one has been popped: it read
    // lexerStack.back() of an empty vector (found by the nightly fuzz).
    // slang v12 stringifies without a lexer; this guards against a regression.
    Analyzer a;
    const std::string text = "`define S(x) `\"x`\"\n`S(a\n";
    a.open(kUri, text);
    auto state = a.get_state(kUri);
    REQUIRE(state != nullptr);
    CHECK(state->tree != nullptr);
}

TEST_CASE("doc sync: change updates text and re-parses tree", "[sync]") {
    Analyzer a;
    a.open(kUri, kSrc1);
    auto s1 = a.get_state(kUri);
    REQUIRE(s1 != nullptr);

    a.change(kUri, kSrc2);
    auto s2 = a.get_state(kUri);
    REQUIRE(s2 != nullptr);
    CHECK(s2->text == kSrc2);
    // New snapshot — different object
    CHECK(s2.get() != s1.get());
}

TEST_CASE("doc sync: close removes DocumentState", "[sync]") {
    Analyzer a;
    a.open(kUri, kSrc1);
    REQUIRE(a.get_state(kUri) != nullptr);

    a.close(kUri);
    CHECK(a.get_state(kUri) == nullptr);
}

TEST_CASE("doc sync: concurrent reads are safe", "[sync]") {
    Analyzer a;
    a.open(kUri, kSrc1);

    std::atomic<int> errors{0};
    std::vector<std::thread> readers;
    for (int i = 0; i < 8; ++i) {
        readers.emplace_back([&] {
            for (int j = 0; j < 100; ++j) {
                auto s = a.get_state(kUri);
                if (s && s->tree == nullptr) ++errors;
            }
        });
    }
    // One writer thread interleaving changes
    std::thread writer([&] {
        for (int j = 0; j < 20; ++j) {
            a.change(kUri, j % 2 == 0 ? kSrc1 : kSrc2);
        }
    });

    for (auto& t : readers) t.join();
    writer.join();

    CHECK(errors == 0);
    CHECK(a.get_state(kUri) != nullptr);
}

// The parse worker holds a filelist file's live project shard back until the
// parse queue has been idle for a moment, so a burst of keystrokes rebuilds it
// once instead of once per character.  Held back is not dropped: this pins that
// the shard does arrive once the typing stops.
TEST_CASE("doc sync: a deferred live shard still reaches the project index", "[sync]") {
    const auto dir = std::filesystem::temp_directory_path() / "lazyverilog-deferred-shard";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const auto path = dir / "blk.sv";
    {
        std::ofstream out(path, std::ios::binary);
        out << "module blk;\nendmodule\n";
    }
    const auto uri = "file://" + path.string();

    Analyzer a;
    a.set_project_config({}, {dir.string()}, {path.string()});
    a.wait_for_background_index_idle();

    a.enqueue_parse(uri, "module blk_renamed;\nendmodule\n");

    auto shard_has_module = [&](const std::string& name) {
        auto snapshot = a.extra_index_snapshot_ptr();
        for (const auto& entry : *snapshot) {
            if (entry.uri != uri)
                continue;
            for (const auto& module : entry.index_ref().modules) {
                if (module.name == name)
                    return true;
            }
        }
        return false;
    };

    // Generous: the deferral is time-based, and a loaded CI runner may need
    // several rounds before the worker sees an idle queue.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline && !shard_has_module("blk_renamed"))
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

    CHECK(shard_has_module("blk_renamed"));

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// The ExtraFileInfo snapshot (hover, definition) is patched, not rebuilt, when
// a listed file's live shard replaces its filelist shard.  The patched entry
// must carry the new shard and the new parse.
TEST_CASE("doc sync: a deferred live shard reaches the extra file snapshot", "[sync]") {
    const auto dir = std::filesystem::temp_directory_path() / "lazyverilog-deferred-file-shard";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const auto path = dir / "blk.sv";
    {
        std::ofstream out(path, std::ios::binary);
        out << "module blk;\nendmodule\n";
    }
    // The URI the filelist entry is keyed by, so the edit replaces its shard.
    const auto uri = uri_from_path(path);

    Analyzer a;
    a.set_project_config({}, {dir.string()}, {path.string()});
    a.wait_for_background_index_idle();
    REQUIRE(a.extra_file_snapshot_ptr()->size() == 1); // cached before the edit

    a.enqueue_parse(uri, "module blk_renamed;\nendmodule\n");

    auto entry_has_module = [&](const std::string& name) {
        auto snapshot = a.extra_file_snapshot_ptr();
        for (const auto& entry : *snapshot) {
            if (entry.uri != uri || entry.state != a.get_state(uri))
                continue;
            for (const auto& module : entry.index_ref().modules) {
                if (module.name == name)
                    return true;
            }
        }
        return false;
    };

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline && !entry_has_module("blk_renamed"))
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

    CHECK(entry_has_module("blk_renamed"));

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// Patching the snapshot must not keep the parse it was first built from: that
// pins a whole SyntaxTree per listed open file for as long as the user types.
TEST_CASE("doc sync: edits to a listed file release its old parse", "[sync]") {
    const auto dir = std::filesystem::temp_directory_path() / "lazyverilog-snapshot-release";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const auto path = dir / "blk.sv";
    const std::string text = "module blk;\n  logic sig;\n  assign sig = 1'b0;\nendmodule\n";
    {
        std::ofstream out(path, std::ios::binary);
        out << text;
    }
    const auto uri = uri_from_path(path);

    Analyzer a;
    a.set_project_config({}, {dir.string()}, {path.string()});
    a.wait_for_background_index_idle();
    a.open(uri, text);
    std::weak_ptr<const DocumentState> first = a.get_state(uri);
    REQUIRE(!first.expired());
    a.symbol_at(uri, 2, 9); // builds and ranks the snapshot

    for (int edit = 1; edit <= 3; ++edit) {
        a.enqueue_parse(uri, text + "// edit " + std::to_string(edit) + "\n");
        const auto parsed = a.get_parsed_state(uri, std::chrono::seconds(10));
        REQUIRE(parsed != nullptr);
        REQUIRE(parsed->tree != nullptr);
        a.symbol_at(uri, 2, 9);
    }

    CHECK(first.expired());

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// ── Diagnostic deduplication ────────────────────────────────────────────────
// A macro body is expanded once per invocation, and slang reports a problem
// inside that body once per expansion — every copy mapped back to the same
// invocation location.  The user has one line to fix, so the server publishes
// each distinct problem once.

TEST_CASE("diagnostics: identical parse diagnostics from one macro invocation collapse",
          "[diagnostics]") {
    const std::string uri = "file:///dup_diag.sv";
    const std::string src = R"SV(`define USES_MISSING(w) \
  logic [`MISSING_W-1:0] a_``w; \
  logic [`MISSING_W-1:0] b_``w; \
  logic [`MISSING_W-1:0] c_``w;

module dup_diag;
  `USES_MISSING(x)
endmodule
)SV";

    Analyzer a;
    a.open(uri, src);
    auto state = a.get_state(uri);
    REQUIRE(state != nullptr);

    const auto count_missing = [](const std::vector<ParseDiagInfo>& diags) {
        return std::count_if(diags.begin(), diags.end(), [](const ParseDiagInfo& d) {
            return d.message.find("MISSING_W") != std::string::npos;
        });
    };

    // The raw parse really does report it once per expansion.
    auto diags = state->parse_diagnostics;
    REQUIRE(count_missing(diags) > 1);

    dedup_parse_diagnostics(diags);
    CHECK(count_missing(diags) == 1);
}

TEST_CASE("diagnostics: dedup keeps distinct positions, severities and messages",
          "[diagnostics]") {
    std::vector<ParseDiagInfo> diags{
        {4, 2, 1, "same", {}},
        {4, 2, 1, "same", {}},
        {4, 2, 1, "different message", {}},
        {4, 2, 2, "same", {}},
        {4, 9, 1, "same", {}},
        {9, 2, 1, "same", {}},
    };

    dedup_parse_diagnostics(diags);

    REQUIRE(diags.size() == 5);
    // First of each duplicate group is kept, in the original order.
    CHECK(diags[0].message == "same");
    CHECK(diags[0].severity == 1);
    CHECK(diags[1].message == "different message");
    CHECK(diags[2].severity == 2);
    CHECK(diags[3].col == 9);
    CHECK(diags[4].line == 9);
}

namespace {

/// A package that `include`s its macros and then a class file using them: the
/// shape of every UVM agent, where the class file means nothing on its own.
struct IncluderTree {
    std::filesystem::path root;

    explicit IncluderTree(const std::string& tag) {
        root = std::filesystem::temp_directory_path() / ("lazyverilog-includer-" + tag);
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root / "inc");
        std::filesystem::create_directories(root / "rtl");
        root = std::filesystem::canonical(root);
        write(root / "inc" / "defs.svh",
              "`define ITEM_W 8\n`define ITEM_LOG(msg) $display(msg)\n");
        write(item(), item_text());
        write(root / "inc" / "orphan.svh", item_text());
        write(pkg(), pkg_text());
    }
    ~IncluderTree() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    static void write(const std::filesystem::path& path, const std::string& text) {
        std::ofstream(path, std::ios::binary) << text;
    }
    std::filesystem::path item() const { return root / "inc" / "item.svh"; }
    std::filesystem::path pkg() const { return root / "rtl" / "pkg.sv"; }
    static std::string item_text() {
        return "class item;\n"
               "  logic [`ITEM_W-1:0] data;\n"
               "  function void show();\n"
               "    `ITEM_LOG(\"x\");\n"
               "  endfunction\n"
               "endclass\n";
    }
    static std::string pkg_text() {
        return "package pkg;\n`include \"defs.svh\"\n`include \"item.svh\"\nendpackage\n";
    }
};

long unknown_macro_count(const DocumentState& state) {
    return std::count_if(state.parse_diagnostics.begin(), state.parse_diagnostics.end(),
                         [](const ParseDiagInfo& d) {
                             return d.message.find("unknown macro") != std::string::npos;
                         });
}

} // namespace

TEST_CASE("an open header parses with the macros its includer defined first",
          "[sync][includer-context]") {
    IncluderTree tree("indexed");
    Analyzer a;
    a.set_project_index_publish_debounce_ms(0);
    a.set_include_dirs({(tree.root / "inc").string()});
    a.set_extra_files({tree.pkg().string()});
    a.wait_for_background_index_idle();

    const auto item_uri = uri_from_path(tree.item());
    a.open(item_uri, IncluderTree::item_text());
    const auto state = a.get_state(item_uri);
    REQUIRE(state != nullptr);
    CHECK(unknown_macro_count(*state) == 0);
    CHECK(std::none_of(state->parse_diagnostics.begin(), state->parse_diagnostics.end(),
                       [](const ParseDiagInfo& d) { return d.severity == 1; }));

    // The same text with nothing including it still reports the macros, so this
    // is the includer's context and not a blanket suppression.
    const auto orphan_uri = uri_from_path(tree.root / "inc" / "orphan.svh");
    a.open(orphan_uri, IncluderTree::item_text());
    const auto orphan = a.get_state(orphan_uri);
    REQUIRE(orphan != nullptr);
    CHECK(unknown_macro_count(*orphan) == 2);
}

TEST_CASE("a header opened before its includer gets the includer's macros once it parses",
          "[sync][includer-context]") {
    // An editor restoring a session opens the class file and the package in
    // whatever order, before any index exists.
    IncluderTree tree("open-order");
    Analyzer a;
    a.set_include_dirs({(tree.root / "inc").string()});

    const auto item_uri = uri_from_path(tree.item());
    a.enqueue_parse(item_uri, IncluderTree::item_text());
    const auto first = a.get_parsed_state(item_uri, std::chrono::seconds(10));
    REQUIRE(first != nullptr);
    REQUIRE(unknown_macro_count(*first) == 2);

    a.enqueue_parse(uri_from_path(tree.pkg()), IncluderTree::pkg_text());
    std::shared_ptr<const DocumentState> now;
    for (int i = 0; i < 500; ++i) {
        now = a.get_state(item_uri);
        if (now && now->tree && unknown_macro_count(*now) == 0)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    REQUIRE(now != nullptr);
    CHECK(now->tree != nullptr);
    CHECK(unknown_macro_count(*now) == 0);
}

namespace {

/// Files under a fresh temp root, indexed from one package the way a `.f`
/// names it.
struct HeaderTree {
    std::filesystem::path root;

    explicit HeaderTree(const std::string& tag) {
        root = std::filesystem::temp_directory_path() / ("lazyverilog-hdr-" + tag);
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root / "inc");
        std::filesystem::create_directories(root / "src");
        root = std::filesystem::canonical(root);
    }
    ~HeaderTree() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
    std::string write(const std::string& relative, const std::string& text) const {
        IncluderTree::write(root / relative, text);
        return text;
    }
    std::string uri(const std::string& relative) const { return uri_from_path(root / relative); }
    void index(Analyzer& a) const {
        a.set_project_index_publish_debounce_ms(0);
        a.set_include_dirs({(root / "inc").string()});
        a.set_extra_files({(root / "src" / "pkg.sv").string()});
        a.wait_for_background_index_idle();
    }
};

/// An inactive file -- every line skipped by a false `ifndef -- parses to an
/// end-of-file token and nothing else.
bool parses_active(const DocumentState& state) {
    return state.tree && state.tree->root().getFirstToken().kind != slang::parsing::TokenKind::EndOfFile;
}

const std::string kDrvText = "`ifndef DRV_SVH\n"
                             "`define DRV_SVH\n"
                             "class drv;\n"
                             "  logic [`M_W-1:0] x;\n"
                             "  function void f();\n"
                             "    `LOG(\"x\");\n"
                             "  endfunction\n"
                             "endclass\n"
                             "`endif\n";

void write_agent_tree(const HeaderTree& tree, bool direct_too) {
    tree.write("inc/macros.svh", "`define M_W 8\n`define LOG(x) $display(x)\n");
    tree.write("inc/agent.svh", "`ifndef AGENT_SVH\n`define AGENT_SVH\n`include \"drv.svh\"\n"
                                "class agent;\n  drv d;\nendclass\n`endif\n");
    tree.write("inc/drv.svh", kDrvText);
    tree.write("src/pkg.sv", std::string("package pkg;\n`include \"macros.svh\"\n`include \"agent.svh\"\n") +
                                 (direct_too ? "`include \"drv.svh\"\n" : "") + "endpackage\n");
}

} // namespace

TEST_CASE("a guarded header included through another header and then directly stays active",
          "[sync][includer-context]") {
    // By its direct `include the package has already defined DRV_SVH, through
    // agent.svh.  Handing that to the header would skip all of it.
    HeaderTree tree("guard");
    write_agent_tree(tree, true);
    Analyzer a;
    tree.index(a);
    a.open(tree.uri("inc/drv.svh"), kDrvText);
    const auto state = a.get_state(tree.uri("inc/drv.svh"));
    REQUIRE(state != nullptr);
    CHECK(parses_active(*state));
    CHECK(unknown_macro_count(*state) == 0);
}

TEST_CASE("a header included only through another header gets the package's macros",
          "[sync][includer-context]") {
    HeaderTree tree("nested");
    write_agent_tree(tree, false);
    Analyzer a;
    tree.index(a);
    a.open(tree.uri("inc/drv.svh"), kDrvText);
    const auto state = a.get_state(tree.uri("inc/drv.svh"));
    REQUIRE(state != nullptr);
    CHECK(parses_active(*state));
    CHECK(unknown_macro_count(*state) == 0);
}

namespace {

/// The body of @p name as the open header's tree sees it; empty when undefined.
std::string macro_seen(const DocumentState& state, std::string_view name) {
    for (const auto* def : state.tree->getDefinedMacros()) {
        if (def->name.valueText() == name)
            return def->toString();
    }
    return {};
}

} // namespace

TEST_CASE("a macro the includer `undef's before the `include is not handed to the header",
          "[sync][includer-context]") {
    // pkg.sv defines W and U, then agent.svh -- before it reaches drv.svh --
    // `undef's U and redefines W.  The table carried into agent.svh's prefix
    // kept U, so drv.svh parsed with a macro no includer has at that point.
    HeaderTree tree("undef");
    tree.write("inc/macros.svh", "`define W 4\n`define U 1\n");
    tree.write("inc/agent.svh", "`undef U\n`undef W\n`define W 16\n`include \"drv.svh\"\n");
    const auto drv = tree.write("inc/drv.svh", "class drv;\n"
                                               "  logic [`W-1:0] d;\n"
                                               "`ifdef U\n  int u_seen;\n`endif\n"
                                               "endclass\n");
    tree.write("src/pkg.sv",
               "package pkg;\n`include \"macros.svh\"\n`include \"agent.svh\"\nendpackage\n");
    Analyzer a;
    tree.index(a);
    a.open(tree.uri("inc/drv.svh"), drv);
    const auto state = a.get_state(tree.uri("inc/drv.svh"));
    REQUIRE(state != nullptr);
    REQUIRE(state->tree != nullptr);
    CHECK(macro_seen(*state, "U").empty());
    CHECK(macro_seen(*state, "W").find("16") != std::string::npos);
}

TEST_CASE("a header's own `define wins over the includer's", "[sync][includer-context]") {
    // slang gives a predefine precedence over a later `define of the same name,
    // so passing the includer's W would silently replace the header's.
    HeaderTree tree("redef");
    tree.write("inc/defs.svh", "`define W 4\n");
    const auto item = tree.write("inc/item.svh", "`define W 16\nclass item;\n  logic [`W-1:0] d;\nendclass\n");
    tree.write("src/pkg.sv", "package pkg;\n`include \"defs.svh\"\n`include \"item.svh\"\nendpackage\n");
    Analyzer a;
    tree.index(a);
    a.open(tree.uri("inc/item.svh"), item);
    const auto state = a.get_state(tree.uri("inc/item.svh"));
    REQUIRE(state != nullptr);
    REQUIRE(state->tree != nullptr);
    std::string w;
    for (const auto* def : state->tree->getDefinedMacros()) {
        if (def->name.valueText() == "W")
            w = def->toString();
    }
    CHECK(w.find("16") != std::string::npos);
}

TEST_CASE("an open header is handed only the includer macros it uses",
          "[sync][includer-context]") {
    // All of UVM is ~280 KB of `define to lex on every keystroke; a class file
    // uses a few of them, and whatever those use in turn.
    HeaderTree tree("filter");
    tree.write("inc/defs.svh", "`define WIDTH 8\n"
                               "`define DECL(n) logic [`WIDTH-1:0] n;\n"
                               "`define UNUSED_ONE 1\n"
                               "`define RUN(F) `F(x)\n"
                               "`define ITEM_ADD(v) v = v + 1;\n");
    const auto item = tree.write("inc/item.svh", "class item;\n"
                                                 "  `DECL(a)\n"
                                                 "  logic [7:0] x;\n"
                                                 "  function void f();\n"
                                                 "    `RUN(ITEM_ADD)\n"
                                                 "  endfunction\n"
                                                 "endclass\n");
    tree.write("src/pkg.sv", "package pkg;\n`include \"defs.svh\"\n`include \"item.svh\"\nendpackage\n");
    Analyzer a;
    tree.index(a);
    a.open(tree.uri("inc/item.svh"), item);
    const auto state = a.get_state(tree.uri("inc/item.svh"));
    REQUIRE(state != nullptr);
    REQUIRE(state->includer_predefine != nullptr);
    const auto& predefine = *state->includer_predefine;
    CHECK(predefine.find("`define DECL") != std::string::npos);
    // Used only by DECL's body.
    CHECK(predefine.find("`define WIDTH") != std::string::npos);
    CHECK(predefine.find("UNUSED_ONE") == std::string::npos);
    // Named only by RUN's argument: slang asks for it, and gets it.
    CHECK(predefine.find("`define ITEM_ADD") != std::string::npos);
    CHECK(unknown_macro_count(*state) == 0);

    // A keystroke that uses no new macro keeps the same predefine.
    a.change(tree.uri("inc/item.svh"), item + "\n");
    const auto edited = a.get_state(tree.uri("inc/item.svh"));
    REQUIRE(edited != nullptr);
    CHECK(edited->includer_predefine == state->includer_predefine);
}

TEST_CASE("a header that includes itself is not reparsed forever", "[sync][includer-context]") {
    // It is in its own include list, which made it look like a header with an
    // includer nobody had looked at -- after every parse, its own included.
    HeaderTree tree("self");
    const std::string text = "`ifndef SELF_SVH\n`define SELF_SVH\n`include \"self.svh\"\n"
                             "class self_c;\nendclass\n`endif\n";
    tree.write("inc/self.svh", text);
    Analyzer a;
    a.set_include_dirs({(tree.root / "inc").string()});
    const auto uri = tree.uri("inc/self.svh");
    a.enqueue_parse(uri, text);
    REQUIRE(a.get_parsed_state(uri, std::chrono::seconds(10)) != nullptr);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const auto before = a.get_state(uri);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const auto after = a.get_state(uri);
    REQUIRE(before != nullptr);
    CHECK(after == before);
}
namespace {

/// The text of the definition of @p name the parse of @p state ended with.
std::string defined_macro(const DocumentState& state, std::string_view name) {
    std::string text;
    for (const auto* def : state.tree->getDefinedMacros()) {
        if (def->name.valueText() == name)
            text = def->toString();
    }
    return text;
}

} // namespace

TEST_CASE("a header's own include sees the includer's macros its conditions test",
          "[sync][includer-context]") {
    // item.svh never names FEATURE; sub.svh, which it includes, tests it.
    HeaderTree tree("subif");
    tree.write("inc/defs.svh", "`define FEATURE\n");
    tree.write("inc/sub.svh", "`ifdef FEATURE\n`define W2 8\n`else\n`define W2 2\n`endif\n");
    const auto item = tree.write("inc/item.svh", "`include \"sub.svh\"\nclass item;\n  logic [`W2-1:0] d;\nendclass\n");
    tree.write("src/pkg.sv", "package pkg;\n`include \"defs.svh\"\n`include \"item.svh\"\nendpackage\n");
    Analyzer a;
    tree.index(a);
    a.open(tree.uri("inc/item.svh"), item);
    const auto state = a.get_state(tree.uri("inc/item.svh"));
    REQUIRE(state != nullptr);
    REQUIRE(state->tree != nullptr);
    CHECK(defined_macro(*state, "W2").find('8') != std::string::npos);

    // Known from the first parse on: a keystroke parses once, with the same input.
    a.change(tree.uri("inc/item.svh"), item + "\n");
    const auto edited = a.get_state(tree.uri("inc/item.svh"));
    REQUIRE(edited != nullptr);
    CHECK(edited->includer_predefine == state->includer_predefine);
}

TEST_CASE("an includer's value wins over a header's `ifndef default", "[sync][includer-context]") {
    // `ifndef BUS_AW `define BUS_AW 32 is a default for when nobody set it; the
    // package did.  The include guard around it is still the header's own.
    HeaderTree tree("dflt");
    tree.write("inc/defs.svh", "`define BUS_AW 16\n");
    const auto item = tree.write("inc/item.svh", "`ifndef ITEM_SVH\n`define ITEM_SVH\n"
                                                 "`ifndef BUS_AW\n`define BUS_AW 32\n`endif\n"
                                                 "class item;\n  logic [`BUS_AW-1:0] a;\nendclass\n"
                                                 "`endif\n");
    tree.write("src/pkg.sv", "package pkg;\n`include \"defs.svh\"\n`include \"item.svh\"\nendpackage\n");
    Analyzer a;
    tree.index(a);
    a.open(tree.uri("inc/item.svh"), item);
    const auto state = a.get_state(tree.uri("inc/item.svh"));
    REQUIRE(state != nullptr);
    REQUIRE(state->includer_predefine != nullptr);
    CHECK(state->includer_predefine->find("`define BUS_AW") != std::string::npos);
    CHECK(state->includer_predefine->find("ITEM_SVH") == std::string::npos);
    CHECK(parses_active(*state));
    CHECK(defined_macro(*state, "BUS_AW").find("32") == std::string::npos);
}
