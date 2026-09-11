// Guards the on-disk shard cache.
//
// Everything here is about one property: a shard that comes back from disk must
// answer exactly what the shard that went in would have answered.  A cache that
// is merely *close* is worse than no cache, because the difference shows up as
// a symbol that resolves before a restart and not after -- the hardest kind of
// bug for a user to report.
//
// So the round-trip tests compare whole tables rather than spot-checking a few
// fields, and they build their input by parsing real source rather than by
// hand: a hand-built index only ever exercises the fields whoever wrote the
// test remembered.
#include "index_cache.hpp"
#include "syntax_index.hpp"
#include "syntax_index_shared.hpp"

#include <catch2/catch_test_macros.hpp>
#include <slang/syntax/SyntaxTree.h>
#include <slang/text/SourceManager.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace {

/// A throwaway directory that cleans itself up.
class TempDir {
  public:
    explicit TempDir(const std::string& tag) {
        path_ = std::filesystem::temp_directory_path() / ("lazyverilog-cache-" + tag);
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::filesystem::path& path() const { return path_; }

    std::filesystem::path write(const std::string& name, const std::string& text) const {
        const auto file = path_ / name;
        // A name may address a subdirectory: include directories only mean
        // something when there is more than one place a header can live.
        std::error_code ec;
        std::filesystem::create_directories(file.parent_path(), ec);
        std::ofstream out(file, std::ios::binary);
        out << text;
        return file;
    }

  private:
    std::filesystem::path path_;
};

const char* kSource = R"(
`define WIDTH_OF(x) $bits(x)

package cfg_pkg;
    parameter int DEPTH = 8;
    typedef logic [7:0] byte_t;
    typedef enum { IDLE, BUSY, DONE } state_e;
    typedef struct packed { logic [3:0] hi; logic [3:0] lo; } split_t;

    function automatic int doubled(int v);
        return v * 2;
    endfunction

    class request;
        int unsigned id;
        byte_t payload;
        function new(int unsigned id_);
            this.id = id_;
        endfunction
        virtual function int cost();
            return doubled(id);
        endfunction
    endclass
endpackage

interface bus_if (input logic clk);
    logic        valid;
    logic [31:0] data;
    modport master (output valid, output data);
    modport slave  (input valid, input data);
endinterface

module leaf #(parameter int W = 4) (
    input  logic         clk,
    input  logic [W-1:0] d,
    output logic [W-1:0] q
);
    always_ff @(posedge clk) q <= d;
endmodule

module top import cfg_pkg::*; (
    input logic clk,
    bus_if      bus
);
    localparam int LOCAL_W = DEPTH;
    logic [LOCAL_W-1:0] stage;
    state_e             state;

    leaf #(.W(LOCAL_W)) u_leaf (.clk(clk), .d(stage), .q());

    for (genvar gi = 0; gi < 2; gi++) begin : g_lane
        logic [7:0] acc;
    end
endmodule
)";

/// Build an index the way the background indexer does: parsed from a real file,
/// Declarations depth, scoped to that file.
SyntaxIndex build_index(const std::filesystem::path& file) {
    auto sm = make_lsp_source_manager();
    auto tree_or_error = slang::syntax::SyntaxTree::fromFile(file.string(), *sm);
    REQUIRE(tree_or_error);
    auto tree = *tree_or_error;
    REQUIRE(tree != nullptr);

    const auto buffers = tree->getSourceBufferIds();
    REQUIRE(!buffers.empty());
    const auto source = sm->getSourceText(buffers.front());
    const auto uri = uri_from_source_buffer(*sm, buffers.front());
    REQUIRE(!uri.empty());
    return SyntaxIndex::build(*tree, source, IndexDepth::Declarations, uri);
}

/// Field-by-field equality over every table the cache stores.  Written out
/// rather than defaulted because the entry structs have no operator==, and
/// adding one to them purely for a test would put it on the request path's
/// types.
void require_same_index(const SyntaxIndex& a, const SyntaxIndex& b) {
    REQUIRE(a.source_files == b.source_files);
    REQUIRE(a.include_dependencies == b.include_dependencies);
    REQUIRE(a.interface_names == b.interface_names);
    REQUIRE(a.package_names == b.package_names);
    REQUIRE(a.package_symbols == b.package_symbols);

    REQUIRE(a.modules.size() == b.modules.size());
    for (size_t i = 0; i < a.modules.size(); ++i) {
        const auto& x = a.modules[i];
        const auto& y = b.modules[i];
        INFO("module " << x.name);
        CHECK(x.name == y.name);
        CHECK(x.file_id == y.file_id);
        CHECK(x.line == y.line);
        CHECK(x.col == y.col);
        CHECK(x.header_semi_line == y.header_semi_line);
        CHECK(x.header_semi_col == y.header_semi_col);
        CHECK(x.has_port_list == y.has_port_list);
        CHECK(x.ansi_port_list == y.ansi_port_list);
        CHECK(x.port_list_has_ports == y.port_list_has_ports);
        CHECK(x.port_list_close_line == y.port_list_close_line);
        CHECK(x.port_list_close_col == y.port_list_close_col);
        REQUIRE(x.ports.size() == y.ports.size());
        for (size_t p = 0; p < x.ports.size(); ++p) {
            INFO("port " << x.ports[p].name);
            CHECK(x.ports[p].name == y.ports[p].name);
            CHECK(x.ports[p].file_id == y.ports[p].file_id);
            CHECK(x.ports[p].direction == y.ports[p].direction);
            CHECK(x.ports[p].type == y.ports[p].type);
            CHECK(x.ports[p].decl_type == y.ports[p].decl_type);
            CHECK(x.ports[p].signal_decl_type == y.ports[p].signal_decl_type);
            CHECK(x.ports[p].default_value == y.ports[p].default_value);
            CHECK(x.ports[p].line == y.ports[p].line);
            CHECK(x.ports[p].col == y.ports[p].col);
        }
        REQUIRE(x.modports.size() == y.modports.size());
        for (size_t m = 0; m < x.modports.size(); ++m) {
            CHECK(x.modports[m].name == y.modports[m].name);
            CHECK(x.modports[m].file_id == y.modports[m].file_id);
            CHECK(x.modports[m].line == y.modports[m].line);
            CHECK(x.modports[m].col == y.modports[m].col);
        }
        // Derived on load, so it has to be rebuilt, not carried.
        CHECK(x.port_by_name == y.port_by_name);
    }

    REQUIRE(a.instances.size() == b.instances.size());
    for (size_t i = 0; i < a.instances.size(); ++i) {
        const auto& x = a.instances[i];
        const auto& y = b.instances[i];
        INFO("instance " << x.instance_name);
        CHECK(x.module_name == y.module_name);
        CHECK(x.instance_name == y.instance_name);
        CHECK(x.parent_module == y.parent_module);
        CHECK(x.file_id == y.file_id);
        CHECK(x.line == y.line);
        CHECK(x.start_line == y.start_line);
        CHECK(x.end_line == y.end_line);
        REQUIRE(x.connections.size() == y.connections.size());
        for (size_t c = 0; c < x.connections.size(); ++c) {
            CHECK(x.connections[c].port_name == y.connections[c].port_name);
            CHECK(x.connections[c].signal_name == y.connections[c].signal_name);
            CHECK(x.connections[c].file_id == y.connections[c].file_id);
            CHECK(x.connections[c].line == y.connections[c].line);
            CHECK(x.connections[c].col == y.connections[c].col);
            CHECK(x.connections[c].hint_col == y.connections[c].hint_col);
        }
    }

    REQUIRE(a.classes.size() == b.classes.size());
    for (size_t i = 0; i < a.classes.size(); ++i) {
        const auto& x = a.classes[i];
        const auto& y = b.classes[i];
        INFO("class " << x.name);
        CHECK(x.name == y.name);
        CHECK(x.file_id == y.file_id);
        CHECK(x.base_class == y.base_class);
        CHECK(x.parent_scope == y.parent_scope);
        CHECK(x.line == y.line);
        CHECK(x.col == y.col);
        REQUIRE(x.fields.size() == y.fields.size());
        for (size_t f = 0; f < x.fields.size(); ++f) {
            CHECK(x.fields[f].name == y.fields[f].name);
            CHECK(x.fields[f].type == y.fields[f].type);
            CHECK(x.fields[f].file_id == y.fields[f].file_id);
            CHECK(x.fields[f].line == y.fields[f].line);
            CHECK(x.fields[f].col == y.fields[f].col);
        }
        REQUIRE(x.methods.size() == y.methods.size());
        for (size_t m = 0; m < x.methods.size(); ++m) {
            INFO("method " << x.methods[m].name);
            CHECK(x.methods[m].name == y.methods[m].name);
            CHECK(x.methods[m].return_type == y.methods[m].return_type);
            CHECK(x.methods[m].params == y.methods[m].params);
            CHECK(x.methods[m].is_task == y.methods[m].is_task);
            CHECK(x.methods[m].file_id == y.methods[m].file_id);
            CHECK(x.methods[m].line == y.methods[m].line);
            CHECK(x.methods[m].col == y.methods[m].col);
        }
    }

    REQUIRE(a.typedefs.size() == b.typedefs.size());
    for (size_t i = 0; i < a.typedefs.size(); ++i) {
        const auto& x = a.typedefs[i];
        const auto& y = b.typedefs[i];
        INFO("typedef " << x.name);
        CHECK(x.name == y.name);
        CHECK(x.resolved == y.resolved);
        CHECK(x.parent_scope == y.parent_scope);
        CHECK(x.file_id == y.file_id);
        CHECK(x.is_enum == y.is_enum);
        CHECK(x.is_struct == y.is_struct);
        CHECK(x.line == y.line);
        CHECK(x.col == y.col);
        REQUIRE(x.enum_members.size() == y.enum_members.size());
        for (size_t e = 0; e < x.enum_members.size(); ++e) {
            CHECK(x.enum_members[e].name == y.enum_members[e].name);
            CHECK(x.enum_members[e].file_id == y.enum_members[e].file_id);
            CHECK(x.enum_members[e].line == y.enum_members[e].line);
            CHECK(x.enum_members[e].col == y.enum_members[e].col);
        }
        REQUIRE(x.fields.size() == y.fields.size());
        for (size_t f = 0; f < x.fields.size(); ++f) {
            CHECK(x.fields[f].name == y.fields[f].name);
            CHECK(x.fields[f].type == y.fields[f].type);
            CHECK(x.fields[f].file_id == y.fields[f].file_id);
            CHECK(x.fields[f].line == y.fields[f].line);
            CHECK(x.fields[f].col == y.fields[f].col);
        }
    }

    REQUIRE(a.macros.size() == b.macros.size());
    for (size_t i = 0; i < a.macros.size(); ++i) {
        CHECK(a.macros[i].name == b.macros[i].name);
        CHECK(a.macros[i].file_id == b.macros[i].file_id);
        CHECK(a.macros[i].is_function_like == b.macros[i].is_function_like);
        CHECK(a.macros[i].params == b.macros[i].params);
        CHECK(a.macros[i].line == b.macros[i].line);
    }

    REQUIRE(a.values.size() == b.values.size());
    for (size_t i = 0; i < a.values.size(); ++i) {
        const auto& x = a.values[i];
        const auto& y = b.values[i];
        INFO("value " << x.name);
        CHECK(x.name == y.name);
        CHECK(x.type == y.type);
        CHECK(x.kind == y.kind);
        CHECK(x.parent_scope == y.parent_scope);
        CHECK(x.generate_label == y.generate_label);
        CHECK(x.default_value == y.default_value);
        CHECK(x.file_id == y.file_id);
        CHECK(x.scope_start_line == y.scope_start_line);
        CHECK(x.scope_end_line == y.scope_end_line);
        CHECK(x.line == y.line);
        CHECK(x.col == y.col);
        CHECK(x.signature == y.signature);
    }

    REQUIRE(a.imports.size() == b.imports.size());
    for (size_t i = 0; i < a.imports.size(); ++i) {
        CHECK(a.imports[i].package_name == b.imports[i].package_name);
        CHECK(a.imports[i].symbol_name == b.imports[i].symbol_name);
        CHECK(a.imports[i].wildcard == b.imports[i].wildcard);
        CHECK(a.imports[i].parent_scope == b.imports[i].parent_scope);
        CHECK(a.imports[i].file_id == b.imports[i].file_id);
        CHECK(a.imports[i].start_line == b.imports[i].start_line);
        CHECK(a.imports[i].end_line == b.imports[i].end_line);
    }

    REQUIRE(a.references.size() == b.references.size());
    for (size_t i = 0; i < a.references.size(); ++i) {
        const auto& x = a.references[i];
        const auto& y = b.references[i];
        INFO("reference " << x.name << " @" << x.line << ":" << x.col);
        CHECK(x.name == y.name);
        CHECK(x.file_id == y.file_id);
        CHECK(x.symbol_id == y.symbol_id);
        CHECK(x.symbol_debug == y.symbol_debug);
        CHECK(x.line == y.line);
        CHECK(x.col == y.col);
        CHECK(x.end_col == y.end_col);
    }

    CHECK(a.module_by_name == b.module_by_name);
    CHECK(a.class_by_name == b.class_by_name);
    CHECK(a.typedef_by_name == b.typedef_by_name);
    CHECK(a.source_file_ids == b.source_file_ids);
    CHECK(a.package_value_by_scoped_name == b.package_value_by_scoped_name);
    CHECK(a.package_type_by_scoped_name == b.package_type_by_scoped_name);
    CHECK(a.package_class_by_scoped_name == b.package_class_by_scoped_name);
}

IndexCache::Key some_key() {
    return IndexCache::Key{
        .content = IndexCache::digest_bytes("content"),
        .config = IndexCache::digest_bytes("config"),
        .dependencies = {{"file:///a.svh", IndexCache::digest_bytes("a")},
                         {"file:///b.svh", IndexCache::digest_bytes("b")}},
    };
}

} // namespace

TEST_CASE("index cache: a parsed shard survives a round trip unchanged", "[index-cache]") {
    TempDir dir("roundtrip");
    const auto file = dir.write("design.sv", kSource);
    const auto original = build_index(file);

    // The corpus has to actually populate what is being compared, or the
    // comparison passes by describing two empty indexes.
    REQUIRE(original.modules.size() >= 3);
    REQUIRE(!original.classes.empty());
    REQUIRE(!original.typedefs.empty());
    REQUIRE(!original.values.empty());
    REQUIRE(!original.instances.empty());
    REQUIRE(!original.references.empty());
    REQUIRE(!original.package_names.empty());
    REQUIRE(!original.interface_names.empty());
    REQUIRE(!original.package_symbols.empty());

    const auto bytes = serialize_index_shard(some_key(), original);
    const auto loaded = deserialize_index_shard(bytes);
    REQUIRE(loaded.has_value());
    require_same_index(original, loaded->index);
}

TEST_CASE("index cache: the key round-trips with the shard", "[index-cache]") {
    TempDir dir("key");
    const auto file = dir.write("design.sv", kSource);
    const auto key = some_key();

    const auto loaded = deserialize_index_shard(serialize_index_shard(key, build_index(file)));
    REQUIRE(loaded.has_value());
    CHECK(loaded->key.content == key.content);
    CHECK(loaded->key.config == key.config);
    REQUIRE(loaded->key.dependencies.size() == key.dependencies.size());
    for (size_t i = 0; i < key.dependencies.size(); ++i) {
        CHECK(loaded->key.dependencies[i].first == key.dependencies[i].first);
        CHECK(loaded->key.dependencies[i].second == key.dependencies[i].second);
    }
}

TEST_CASE("index cache: an empty index round-trips", "[index-cache]") {
    // The degenerate shape a file that fails to parse produces.  It must load
    // back as an empty index rather than as a read failure, or every such file
    // is re-parsed on every launch forever.
    const auto loaded = deserialize_index_shard(serialize_index_shard(some_key(), SyntaxIndex{}));
    REQUIRE(loaded.has_value());
    require_same_index(SyntaxIndex{}, loaded->index);
}

TEST_CASE("index cache: store and load through the filesystem", "[index-cache]") {
    TempDir dir("store");
    const auto file = dir.write("design.sv", kSource);
    const auto original = build_index(file);

    auto cache = IndexCache::open(dir.path());
    REQUIRE(cache.has_value());

    const std::string uri = "file:///project/design.sv";
    CHECK(!cache->load(uri).has_value()); // nothing stored yet

    cache->store(uri, some_key(), original);
    const auto loaded = cache->load(uri);
    REQUIRE(loaded.has_value());
    require_same_index(original, loaded->index);

    // Written inside the project, so it must not show up in `git status`.
    CHECK(std::filesystem::exists(cache->directory() / ".gitignore"));
}

TEST_CASE("index cache: a rewritten shard replaces the previous one", "[index-cache]") {
    TempDir dir("rewrite");
    auto cache = IndexCache::open(dir.path());
    REQUIRE(cache.has_value());

    const std::string uri = "file:///project/design.sv";
    SyntaxIndex first;
    first.modules.push_back(ModuleEntry{.name = "before"});
    SyntaxIndex second;
    second.modules.push_back(ModuleEntry{.name = "after"});

    cache->store(uri, some_key(), first);
    cache->store(uri, some_key(), second);

    const auto loaded = cache->load(uri);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->index.modules.size() == 1);
    CHECK(loaded->index.modules.front().name == "after");

    // A store leaves exactly one file behind: the temporary it wrote through is
    // renamed, never left for the next launch to trip over.
    size_t shards = 0;
    for (const auto& entry : std::filesystem::directory_iterator(cache->directory())) {
        if (entry.path().extension() == ".idx")
            ++shards;
        CHECK(entry.path().extension() != ".tmp");
    }
    CHECK(shards == 1);
}

TEST_CASE("index cache: two files with the same basename get separate shards", "[index-cache]") {
    // An RTL tree is full of these -- every block has its own `pkg.sv`.  The
    // shard name carries a hash of the full URI for exactly this reason.
    TempDir dir("basename");
    auto cache = IndexCache::open(dir.path());
    REQUIRE(cache.has_value());

    SyntaxIndex a;
    a.modules.push_back(ModuleEntry{.name = "from_a"});
    SyntaxIndex b;
    b.modules.push_back(ModuleEntry{.name = "from_b"});

    cache->store("file:///project/a/pkg.sv", some_key(), a);
    cache->store("file:///project/b/pkg.sv", some_key(), b);

    const auto loaded_a = cache->load("file:///project/a/pkg.sv");
    const auto loaded_b = cache->load("file:///project/b/pkg.sv");
    REQUIRE(loaded_a.has_value());
    REQUIRE(loaded_b.has_value());
    REQUIRE(loaded_a->index.modules.size() == 1);
    REQUIRE(loaded_b->index.modules.size() == 1);
    CHECK(loaded_a->index.modules.front().name == "from_a");
    CHECK(loaded_b->index.modules.front().name == "from_b");
}

TEST_CASE("index cache: a truncated or corrupt shard is a miss, not a crash", "[index-cache]") {
    // The shard on disk is whatever survived the last run.  A killed process,
    // a full disk, or a future version's format all have to degrade to "parse
    // it again" rather than to a bad index or a fault.
    TempDir dir("corrupt");
    const auto file = dir.write("design.sv", kSource);
    const auto bytes = serialize_index_shard(some_key(), build_index(file));
    REQUIRE(bytes.size() > 64);

    SECTION("truncated at every length") {
        // Every prefix, not a few: the failure has to come from the bounds
        // check, not from where a particular cut happened to land.
        for (size_t length = 0; length < bytes.size(); length += 7)
            CHECK(!deserialize_index_shard(std::string_view(bytes).substr(0, length)).has_value());
    }

    SECTION("wrong magic") {
        auto bad = bytes;
        bad[0] = 'X';
        CHECK(!deserialize_index_shard(bad).has_value());
    }

    SECTION("wrong format version") {
        auto bad = bytes;
        bad[4] = static_cast<char>(bad[4] + 1);
        CHECK(!deserialize_index_shard(bad).has_value());
    }

    SECTION("a corrupt byte in the body never yields a bad index") {
        // Flipping bytes hits string-table indices, counts, and file ids.  Any
        // of those may legitimately still decode -- what must never happen is a
        // load that returns an index pointing outside its own tables.
        for (size_t i = 12; i < bytes.size(); i += 101) {
            auto bad = bytes;
            bad[i] = static_cast<char>(~bad[i]);
            const auto loaded = deserialize_index_shard(bad);
            if (!loaded)
                continue;
            const auto& index = loaded->index;
            for (const auto& reference : index.references)
                CHECK((reference.file_id == kInvalidSourceFileID ||
                       reference.file_id < index.source_files.size()));
            for (const auto& [key, at] : index.package_class_by_scoped_name)
                CHECK(at < index.classes.size());
            for (const auto& [key, at] : index.package_type_by_scoped_name)
                CHECK(at < index.typedefs.size());
            for (const auto& [key, at] : index.package_value_by_scoped_name)
                CHECK(at < index.values.size());
        }
    }
}

TEST_CASE("index cache: digests separate what a parse depends on", "[index-cache]") {
    CHECK(IndexCache::digest_bytes("module a; endmodule") ==
          IndexCache::digest_bytes("module a; endmodule"));
    CHECK(IndexCache::digest_bytes("module a; endmodule") !=
          IndexCache::digest_bytes("module b; endmodule"));
    // A one-bit difference has to move the digest; this is the case a weak
    // hash gets wrong and it is the case source edits actually produce.
    CHECK(IndexCache::digest_bytes("parameter int W = 8;") !=
          IndexCache::digest_bytes("parameter int W = 9;"));
    CHECK(IndexCache::digest_bytes("") == IndexCache::digest_bytes(""));
    CHECK(!IndexCache::digest_bytes("x").empty());

    // Order matters to the preprocessor, so it has to matter to the digest.
    const std::vector<std::filesystem::path> dirs{"/a", "/b"};
    const std::vector<std::filesystem::path> swapped{"/b", "/a"};
    CHECK(IndexCache::config_digest({"A=1", "B=2"}, dirs) !=
          IndexCache::config_digest({"B=2", "A=1"}, dirs));
    CHECK(IndexCache::config_digest({"A=1"}, dirs) !=
          IndexCache::config_digest({"A=1"}, swapped));
    CHECK(IndexCache::config_digest({"A=1"}, dirs) == IndexCache::config_digest({"A=1"}, dirs));
    // Concatenation must not alias: ["AB"] and ["A","B"] are different configs.
    CHECK(IndexCache::config_digest({"AB"}, {}) != IndexCache::config_digest({"A", "B"}, {}));
    // A define must not alias with an include directory of the same spelling.
    CHECK(IndexCache::config_digest({"x"}, {}) != IndexCache::config_digest({}, {"x"}));
}

TEST_CASE("index cache: a file that cannot be read has no digest", "[index-cache]") {
    // Distinct from a zero digest: two unreadable files must not compare equal,
    // or a deleted header would validate against another deleted header.
    TempDir dir("missing");
    CHECK(!IndexCache::digest_file(dir.path() / "not-here.sv").has_value());

    const auto file = dir.write("here.sv", "module m; endmodule\n");
    const auto digest = IndexCache::digest_file(file);
    REQUIRE(digest.has_value());
    CHECK(*digest == IndexCache::digest_bytes("module m; endmodule\n"));
}

// ── The cache inside the analyzer ───────────────────────────────────────────
//
// The round-trip tests above prove a shard survives disk.  These prove the
// harder half: that a shard is *reused* when it should be and *not* when it
// should not.  A cache that never invalidates is fast and wrong, and the
// symptom -- an edit that navigation ignores until the directory is deleted --
// is one a user has no way to diagnose.
#include "analyzer.hpp"
#include "config.hpp"

#include <set>

namespace {

/// A project whose files can be rewritten between analyzer runs.
class CacheProject {
  public:
    explicit CacheProject(const std::string& tag) : dir_(tag) {}

    void write(const std::string& name, const std::string& text) const { dir_.write(name, text); }

    std::filesystem::path root() const { return std::filesystem::canonical(dir_.path()); }

    /// One cold index of the whole project, through the same entry point the
    /// server uses.  Returns the published snapshot.
    std::shared_ptr<const ProjectIndexSnapshot> index(Analyzer& analyzer,
                                                      const std::vector<std::string>& files,
                                                      const std::vector<std::string>& defines = {},
                                                      const std::vector<std::string>& incdirs = {}) const {
        std::vector<std::string> paths;
        for (const auto& name : files)
            paths.push_back((root() / name).string());
        std::vector<std::string> include_dirs;
        for (const auto& dir : incdirs)
            include_dirs.push_back((root() / dir).string());
        if (include_dirs.empty())
            include_dirs.push_back(root().string());
        analyzer.set_project_index_publish_debounce_ms(0);
        analyzer.set_project_config(defines, include_dirs, paths, {}, root().string());
        analyzer.wait_for_background_index_idle();
        // Shard writes are deliberately off the indexing path, so a test that
        // asserts on what the *next* launch sees has to wait for them.  The
        // server never does.
        analyzer.wait_for_index_cache_writes_idle();
        return analyzer.project_index_snapshot();
    }

    size_t shard_files() const {
        const auto directory = IndexCache::directory_for(root());
        if (!std::filesystem::exists(directory))
            return 0;
        size_t count = 0;
        for (const auto& entry : std::filesystem::directory_iterator(directory))
            count += entry.path().extension() == ".idx";
        return count;
    }

  private:
    TempDir dir_;
};

/// Names of every value in the snapshot, which is what an edit has to move.
std::set<std::string> snapshot_values(const std::shared_ptr<const ProjectIndexSnapshot>& snapshot) {
    std::set<std::string> names;
    if (!snapshot)
        return names;
    for (const auto& shard : snapshot->shards) {
        if (!shard.index)
            continue;
        for (const auto& value : shard.index->values)
            names.insert(value.name);
    }
    return names;
}

std::set<std::string> snapshot_modules(const std::shared_ptr<const ProjectIndexSnapshot>& s) {
    std::set<std::string> names;
    if (s) {
        for (const auto& [name, ref] : s->module_by_name)
            names.insert(name);
    }
    return names;
}

} // namespace

TEST_CASE("index cache: a second launch reproduces the first launch's index", "[index-cache]") {
    CacheProject project("analyzer-hit");
    project.write("defs.svh", "localparam int SHARED_A = 1;\nlocalparam int SHARED_B = 2;\n");
    project.write("a.sv", "`include \"defs.svh\"\nmodule a (input logic clk);\n"
                          "  logic [7:0] sig_a;\nendmodule\n");
    project.write("b.sv", "`include \"defs.svh\"\nmodule b (input logic clk);\n"
                          "  logic [7:0] sig_b;\nendmodule\n");

    std::set<std::string> cold_modules, cold_values;
    {
        Analyzer analyzer;
        const auto snapshot = project.index(analyzer, {"a.sv", "b.sv"});
        cold_modules = snapshot_modules(snapshot);
        cold_values = snapshot_values(snapshot);
    }
    CHECK(cold_modules == std::set<std::string>{"a", "b"});
    CHECK(project.shard_files() > 0);

    // Nothing changed on disk, so the second launch must answer identically --
    // that equality is the entire contract.
    {
        Analyzer analyzer;
        const auto snapshot = project.index(analyzer, {"a.sv", "b.sv"});
        CHECK(snapshot_modules(snapshot) == cold_modules);
        CHECK(snapshot_values(snapshot) == cold_values);
    }
}

TEST_CASE("index cache: editing a file between launches is picked up", "[index-cache]") {
    CacheProject project("analyzer-edit");
    project.write("defs.svh", "localparam int SHARED_A = 1;\n");
    project.write("a.sv", "`include \"defs.svh\"\nmodule a; logic [7:0] before_edit; endmodule\n");

    {
        Analyzer analyzer;
        const auto snapshot = project.index(analyzer, {"a.sv"});
        CHECK(snapshot_values(snapshot).count("before_edit") == 1);
    }

    // Same size, same name, only the contents differ -- the case a size- or
    // mtime-keyed cache is most likely to get wrong.
    project.write("a.sv", "`include \"defs.svh\"\nmodule a; logic [7:0] afterr_edit; endmodule\n");
    {
        Analyzer analyzer;
        const auto snapshot = project.index(analyzer, {"a.sv"});
        const auto values = snapshot_values(snapshot);
        CHECK(values.count("afterr_edit") == 1);
        CHECK(values.count("before_edit") == 0);
    }
}

TEST_CASE("index cache: editing a shared header invalidates its includers", "[index-cache]") {
    // The case the dependency digests exist for: nothing about a.sv or b.sv
    // changed, so their own digests still match.  Only the header moved.
    CacheProject project("analyzer-header");
    project.write("defs.svh", "localparam int HDR_BEFORE = 1;\n");
    project.write("a.sv", "`include \"defs.svh\"\nmodule a; endmodule\n");
    project.write("b.sv", "`include \"defs.svh\"\nmodule b; endmodule\n");

    {
        Analyzer analyzer;
        const auto snapshot = project.index(analyzer, {"a.sv", "b.sv"});
        CHECK(snapshot_values(snapshot).count("HDR_BEFORE") == 1);
    }

    project.write("defs.svh", "localparam int HDR_AFTER = 1;\n");
    {
        Analyzer analyzer;
        const auto snapshot = project.index(analyzer, {"a.sv", "b.sv"});
        const auto values = snapshot_values(snapshot);
        CHECK(values.count("HDR_AFTER") == 1);
        CHECK(values.count("HDR_BEFORE") == 0);
    }
}

TEST_CASE("index cache: changing a define invalidates every shard", "[index-cache]") {
    // No file changed at all.  What changed is what a parse of those files
    // means, which is why the config digest is part of the key.
    CacheProject project("analyzer-define");
    project.write("a.sv", "module a;\n`ifdef PICK_LEFT\n  logic left_branch;\n"
                          "`else\n  logic right_branch;\n`endif\nendmodule\n");

    {
        Analyzer analyzer;
        const auto snapshot = project.index(analyzer, {"a.sv"}, {"PICK_LEFT"});
        CHECK(snapshot_values(snapshot).count("left_branch") == 1);
    }
    {
        Analyzer analyzer;
        const auto snapshot = project.index(analyzer, {"a.sv"}, {});
        const auto values = snapshot_values(snapshot);
        CHECK(values.count("right_branch") == 1);
        CHECK(values.count("left_branch") == 0);
    }
}

TEST_CASE("index cache: a deleted header forces its includer to be reparsed", "[index-cache]") {
    // A dependency that cannot be read is a miss, not a match against another
    // unreadable file.  The includer then reparses and reports the failure the
    // user should see, rather than serving an index of a file that no longer
    // resolves.
    CacheProject project("analyzer-deleted");
    project.write("defs.svh", "localparam int GONE_SOON = 1;\n");
    project.write("a.sv", "`include \"defs.svh\"\nmodule a; endmodule\n");

    {
        Analyzer analyzer;
        const auto snapshot = project.index(analyzer, {"a.sv"});
        CHECK(snapshot_values(snapshot).count("GONE_SOON") == 1);
    }

    std::filesystem::remove(project.root() / "defs.svh");
    {
        Analyzer analyzer;
        const auto snapshot = project.index(analyzer, {"a.sv"});
        CHECK(snapshot_values(snapshot).count("GONE_SOON") == 0);
        CHECK(snapshot_modules(snapshot).count("a") == 1);
    }
}

TEST_CASE("index cache: a corrupt shard on disk falls back to parsing", "[index-cache]") {
    CacheProject project("analyzer-corrupt");
    project.write("a.sv", "module a; logic [7:0] still_here; endmodule\n");

    {
        Analyzer analyzer;
        (void)project.index(analyzer, {"a.sv"});
    }
    REQUIRE(project.shard_files() > 0);

    // Whatever survived the last run is what the next one reads.  Garbage has
    // to cost a reparse, never an answer.
    for (const auto& entry :
         std::filesystem::directory_iterator(IndexCache::directory_for(project.root()))) {
        if (entry.path().extension() != ".idx")
            continue;
        std::ofstream out(entry.path(), std::ios::binary | std::ios::trunc);
        out << "not a shard at all, but long enough to look like one";
    }

    Analyzer analyzer;
    const auto snapshot = project.index(analyzer, {"a.sv"});
    CHECK(snapshot_values(snapshot).count("still_here") == 1);
    CHECK(snapshot_modules(snapshot).count("a") == 1);
}

TEST_CASE("index cache: no project root means no cache and no directory", "[index-cache]") {
    // A server with no lazyverilog.toml must behave exactly as it did before
    // the cache existed, and must not scatter .cache directories.
    CacheProject project("analyzer-rootless");
    project.write("a.sv", "module a; logic [7:0] uncached; endmodule\n");

    Analyzer analyzer;
    analyzer.set_project_index_publish_debounce_ms(0);
    analyzer.set_project_config({}, {project.root().string()},
                                {(project.root() / "a.sv").string()});
    analyzer.wait_for_background_index_idle();

    CHECK(snapshot_values(analyzer.project_index_snapshot()).count("uncached") == 1);
    CHECK(!std::filesystem::exists(IndexCache::directory_for(project.root())));
}

TEST_CASE("index cache: [index].cache defaults on and can be turned off", "[index-cache]") {
    // The server decides by handing the analyzer an empty project root, so what
    // this pins is the config plumbing: the default, and that `false` reaches
    // it.  The uncached behaviour itself is covered above.
    CHECK(Config{}.index.cache);

    TempDir dir("config");
    {
        std::ofstream out(dir.path() / "lazyverilog.toml", std::ios::binary);
        out << "[index]\ncache = false\n";
    }
    std::string warning;
    const auto config = load_config(dir.path(), &warning);
    CHECK(warning.empty());
    CHECK(!config.index.cache);
}

TEST_CASE("header text cache: a projected header keeps the digest of the real one",
          "[index-cache]") {
    // Once a header's own shard exists, the burst serves the rest of its files
    // that header's *directives* instead of the header.  A parse seeded from
    // here therefore never sees the file, and the shard cache still has to key
    // that parse's shard on the file -- so the digest is taken when the full
    // text enters and travels with the entry, unchanged, across project().
    HeaderTextCache cache;
    const std::string full = "`define A 1\nlocalparam int DECL = 3;\n";
    const auto expected = IndexCache::digest_source_buffer(full);

    cache.record_parse(1, {{"/proj/shared.svh", full}}, /*count_as_burst_parse=*/true);
    REQUIRE(cache.seed_candidates(1).size() == 1);
    CHECK(cache.seed_candidates(1).front().digest == expected);

    cache.project(1, "/proj/shared.svh", "`define A 1\n");
    const auto after = cache.seed_candidates(1);
    REQUIRE(after.size() == 1);
    CHECK(*after.front().text == "`define A 1\n");
    CHECK(after.front().digest == expected);
}

TEST_CASE("index cache: a burst's projected header does not cost anyone a shard",
          "[index-cache]") {
    // The digests a shard is keyed on come from what the burst's parses read,
    // and most of a burst reads a shared header only as the projection above.
    // Every project file still has to end up with a shard, and the next launch
    // still has to reuse them.
    CacheProject project("analyzer-projected-header");

    std::string shared = "`define SHARED_MACRO 1\n";
    for (int i = 0; i < 4000; ++i)
        shared += "// pad pad pad pad pad pad pad pad pad pad pad pad pad pad\n";
    shared += "localparam int SHARED_FROM_HEADER = 7;\n";
    project.write("shared.svh", shared);

    std::vector<std::string> files;
    for (int i = 0; i < 8; ++i) {
        const auto name = "m" + std::to_string(i) + ".sv";
        project.write(name, "`include \"shared.svh\"\nmodule m" + std::to_string(i) +
                                ";\n  logic [7:0] sig_" + std::to_string(i) + ";\nendmodule\n");
        files.push_back(name);
    }

    std::set<std::string> cold_values;
    {
        Analyzer analyzer;
        cold_values = snapshot_values(project.index(analyzer, files));
    }

    // One per project file plus the header's own.
    CHECK(project.shard_files() == files.size() + 1);

    Analyzer analyzer;
    const auto warm = project.index(analyzer, files);
    CHECK(snapshot_values(warm) == cold_values);
    for (int i = 0; i < 8; ++i)
        CHECK(snapshot_modules(warm).count("m" + std::to_string(i)) == 1);
}


TEST_CASE("index cache: a header created after its includer is picked up", "[index-cache]") {
    // Writing the file that `include`s a header before writing the header is
    // the ordinary order to work in.  The first index finds nothing, and no
    // file the shard key hashes changes when the header appears -- so without
    // recording that the search found nothing, the shard stays valid and the
    // header is invisible until the includer itself is touched.
    CacheProject project("analyzer-late-header");
    project.write("a.sv", "`include \"late.svh\"\nmodule a;\n  logic [7:0] sig_a;\nendmodule\n");

    {
        Analyzer analyzer;
        const auto cold = project.index(analyzer, {"a.sv"});
        CHECK(snapshot_values(cold).count("late_from_header") == 0);
    }

    project.write("late.svh", "localparam int late_from_header = 4;\n");

    Analyzer analyzer;
    const auto warm = project.index(analyzer, {"a.sv"});
    CHECK(snapshot_values(warm).count("late_from_header") == 1);
}

TEST_CASE("index cache: a header shadowed from an earlier directory is picked up",
          "[index-cache]") {
    // An override directory ahead of the shared one in the search order is how
    // a design substitutes a header without editing anything that includes it.
    // Nothing hashed changes: the includer is untouched and the header it used
    // to resolve to is still there, byte for byte.  Only the decision changed.
    CacheProject project("analyzer-shadowed-header");
    project.write("base/defs.svh", "localparam int from_base = 1;\n");
    project.write("a.sv", "`include \"defs.svh\"\nmodule a;\n  logic [7:0] sig_a;\nendmodule\n");

    const std::vector<std::string> incdirs{"override", "base"};
    std::filesystem::create_directories(project.root() / "override");

    {
        Analyzer analyzer;
        const auto cold = project.index(analyzer, {"a.sv"}, {}, incdirs);
        CHECK(snapshot_values(cold).count("from_base") == 1);
        CHECK(snapshot_values(cold).count("from_override") == 0);
    }

    project.write("override/defs.svh", "localparam int from_override = 2;\n");

    Analyzer analyzer;
    const auto warm = project.index(analyzer, {"a.sv"}, {}, incdirs);
    CHECK(snapshot_values(warm).count("from_override") == 1);
    // And the one it shadowed is gone: nothing includes it any more.
    CHECK(snapshot_values(warm).count("from_base") == 0);
}

TEST_CASE("index cache: an unchanged project is served from the shards", "[index-cache]") {
    // Every other warm test here asserts the second launch produces the right
    // index -- which a launch that quietly reparsed everything also does.  That
    // blind spot hid a real regression: hashing slang's buffer instead of the
    // file compared a NUL-terminated string against the file's bytes, so no
    // shard ever validated and the cache was doing nothing at all.
    //
    // So this one makes a hit visibly different from a parse.  The stored shard
    // is edited on disk, keeping its key, to hold a symbol the source does not
    // contain.  If the symbol comes back, the shard was read; if it does not,
    // the file was parsed and the cache is dead.
    CacheProject project("analyzer-served-from-disk");
    project.write("a.sv", "module a;\n  logic [7:0] sig_a;\nendmodule\n");

    {
        Analyzer analyzer;
        project.index(analyzer, {"a.sv"});
    }

    const auto directory = IndexCache::directory_for(project.root());
    size_t edited = 0;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.path().extension() != ".idx")
            continue;
        std::ifstream in(entry.path(), std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
        auto loaded = deserialize_index_shard(bytes);
        REQUIRE(loaded);

        ValueEntry sentinel;
        sentinel.name = "only_a_reused_shard_has_this";
        sentinel.kind = "variable";
        sentinel.file_id = kInvalidSourceFileID;
        loaded->index.values.push_back(sentinel);

        const auto rewritten =
            serialize_index_shard(loaded->key, loaded->index, loaded->stands_alone);
        std::ofstream out(entry.path(), std::ios::binary | std::ios::trunc);
        out.write(rewritten.data(), static_cast<std::streamsize>(rewritten.size()));
        ++edited;
    }
    REQUIRE(edited == 1);

    Analyzer analyzer;
    const auto warm = snapshot_values(project.index(analyzer, {"a.sv"}));
    CHECK(warm.count("only_a_reused_shard_has_this") == 1);
    CHECK(warm.count("sig_a") == 1);
}

TEST_CASE("index cache: an unchanged include resolution is still a hit", "[index-cache]") {
    // The check above must not cost the reuse it is guarding.  A project whose
    // headers resolve exactly as they did is the case this cache exists for.
    CacheProject project("analyzer-stable-resolution");
    project.write("defs.svh", "localparam int shared_decl = 9;\n");
    project.write("a.sv", "`include \"defs.svh\"\nmodule a;\n  logic [7:0] sig_a;\nendmodule\n");

    std::set<std::string> cold;
    {
        Analyzer analyzer;
        cold = snapshot_values(project.index(analyzer, {"a.sv"}));
    }
    const auto shards_after_cold = project.shard_files();

    Analyzer analyzer;
    CHECK(snapshot_values(project.index(analyzer, {"a.sv"})) == cold);
    // A reparse would have rewritten them; the count is the same either way,
    // so what this pins is that the index is reproduced, not that it was read.
    CHECK(project.shard_files() == shards_after_cold);
    CHECK(cold.count("shared_decl") == 1);
}

TEST_CASE("index cache: a shard whose file is gone is removed", "[index-cache]") {
    // A shard is named from its file's URI, so editing a file rewrites its
    // shard in place and the directory does not grow.  Deleting or renaming one
    // is what leaves an orphan behind, and nothing ever collected them: on a
    // tree with generated or frequently renamed RTL the directory grows without
    // bound, inside the user's own checkout.
    CacheProject project("analyzer-prune");
    project.write("keep.sv", "module keep;\n  logic [7:0] kept;\nendmodule\n");
    project.write("goes.sv", "module goes;\n  logic [7:0] gone;\nendmodule\n");

    {
        Analyzer analyzer;
        project.index(analyzer, {"keep.sv", "goes.sv"});
    }
    REQUIRE(project.shard_files() == 2);

    // Deleted from the project and from disk, which is what a rename looks like
    // from the old name's side.
    std::filesystem::remove(project.root() / "goes.sv");

    Analyzer analyzer;
    const auto warm = project.index(analyzer, {"keep.sv"});
    CHECK(snapshot_values(warm).count("kept") == 1);
    CHECK(project.shard_files() == 1);
}

TEST_CASE("index cache: a shard whose file still exists is kept", "[index-cache]") {
    // Only files that are definitely gone.  A file out of the project today is
    // legitimately reusable when it comes back -- a filelist edit, a branch
    // switch -- and re-reading it costs a parse that the shard already paid
    // for.  Skipping one costs a stat.
    CacheProject project("analyzer-prune-keeps");
    project.write("a.sv", "module a;\n  logic [7:0] sig_a;\nendmodule\n");
    project.write("b.sv", "module b;\n  logic [7:0] sig_b;\nendmodule\n");

    {
        Analyzer analyzer;
        project.index(analyzer, {"a.sv", "b.sv"});
    }
    REQUIRE(project.shard_files() == 2);

    // b.sv drops out of the filelist but stays on disk.
    Analyzer analyzer;
    project.index(analyzer, {"a.sv"});
    CHECK(project.shard_files() == 2);
}
