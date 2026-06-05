#pragma once
#include <memory>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace slang::syntax {
class SyntaxTree;
}

using SourceFileID = uint32_t;
inline constexpr SourceFileID kInvalidSourceFileID =
    std::numeric_limits<SourceFileID>::max();

/// Compact deterministic identity for a semantic-ish SystemVerilog symbol.
///
/// The project index stores this instead of using raw names as identities.  The
/// canonical spelling is still kept next to references for diagnostics/tests,
/// but hot lookup and equality are two integer comparisons.
///
/// This is intentionally a syntactic semantic ID, not an elaborated design ID:
///
///   module_signal::memory_top::state
///   module::memory
///   port::memory::clk
///
/// That makes it usable while code is partially broken and avoids keeping full
/// project ASTs alive.  The hashing routine is local and deterministic; it can
/// be replaced by XXH3_128 later without changing the data model.
struct SymbolID {
    uint64_t lo{0};
    uint64_t hi{0};

    constexpr bool empty() const { return lo == 0 && hi == 0; }
    constexpr explicit operator bool() const { return !empty(); }
    constexpr bool operator==(const SymbolID&) const = default;

    static SymbolID from_canonical(std::string_view canonical) {
        if (canonical.empty())
            return {};

        // Two seeded FNV-1a streams.  FNV is not chosen as the final hash
        // recommendation; it is a tiny deterministic bootstrap so the index can
        // move from string identity to compact 128-bit identity immediately.
        // Performance-wise this is already far below parse/index walking cost.
        uint64_t h1 = 14695981039346656037ull;
        uint64_t h2 = 1099511628211ull ^ 0x9e3779b97f4a7c15ull;
        for (unsigned char c : canonical) {
            h1 ^= c;
            h1 *= 1099511628211ull;
            h2 ^= static_cast<unsigned char>(c + 0x9d);
            h2 *= 14029467366897019727ull;
        }
        if (h1 == 0 && h2 == 0)
            h2 = 1;
        return SymbolID{h1, h2};
    }
};

struct SymbolIDHash {
    size_t operator()(const SymbolID& id) const {
        return static_cast<size_t>(id.lo ^ (id.hi + 0x9e3779b97f4a7c15ull +
                                            (id.lo << 6) + (id.lo >> 2)));
    }
};

struct PortEntry {
    std::string name;
    SourceFileID file_id{kInvalidSourceFileID};
    std::string direction; // input/output/inout/ref/parameter/localparam
    std::string type;
    // Complete datatype / net declaration prefix for command/UI metadata. This
    // may include a net kind that `type` omits for display compatibility, for
    // example `wire [5:0]` for `input wire [5:0] a`.
    std::string decl_type;
    // Datatype to use when synthesizing an internal bridge signal. This is
    // derived from slang syntax facts, not from string inspection. Net ports
    // become variables, for example `output wire [5:0] o` -> `logic [5:0] s`.
    std::string signal_decl_type;
    // Default value text for parameter/localparam ports (empty for regular ports).
    std::string default_value;
    int line{0}; // 1-based, 0 if unknown
    int col{0};  // 0-based
};

struct ModportEntry {
    std::string name;
    SourceFileID file_id{kInvalidSourceFileID};
    int line{0};
    int col{0};
};

struct ModuleEntry {
    std::string name;
    SourceFileID file_id{kInvalidSourceFileID};
    int line{0}; // 1-based, 0 if unknown
    int col{0};  // 0-based
    // Edit-oriented source facts derived from the current-file AST or from a
    // closed-file SyntaxIndex shard.  These are intentionally small ranges, not
    // retained AST nodes, so cross-file edit features can follow the project
    // model: current files use AST snapshots, closed project files use index
    // facts.  Lines here are 0-based because they feed LSP TextEdit ranges.
    int header_semi_line{-1};
    int header_semi_col{-1};
    bool has_port_list{false};
    bool ansi_port_list{false};
    bool port_list_has_ports{false};
    int port_list_close_line{-1};
    int port_list_close_col{-1};
    std::vector<PortEntry> ports;
    std::vector<ModportEntry> modports;
    std::unordered_map<std::string, size_t> port_by_name;
};

struct NamedPortConn {
    std::string port_name;
    std::string signal_name;
    SourceFileID file_id{kInvalidSourceFileID};
    int line{0};
    int col{0};
    int hint_col{0};
};

struct InstanceEntry {
    std::string module_name;
    std::string instance_name;
    std::string parent_module;
    SourceFileID file_id{kInvalidSourceFileID};
    int line{0};
    int start_line{0}; // 0-based
    int end_line{0};   // 0-based
    std::vector<NamedPortConn> connections;
};

// ── New symbol types for completion ───────────────────────────────────────────

struct FieldEntry {
    std::string name;
    std::string type; // syntactic type text (best-effort)
    SourceFileID file_id{kInvalidSourceFileID};
    int line{0};
    int col{0};
};

struct MethodEntry {
    std::string name;
    std::string return_type;
    bool is_task{false};
    SourceFileID file_id{kInvalidSourceFileID};
    int line{0};
    int col{0};
};

struct ClassEntry {
    std::string name;
    SourceFileID file_id{kInvalidSourceFileID};
    std::string base_class; // empty if no extends clause
    std::string parent_scope; // package/class scope when known
    std::vector<FieldEntry> fields;
    std::vector<MethodEntry> methods;
    int line{0};
    int col{0};
};

struct EnumMemberEntry {
    std::string name;
    SourceFileID file_id{kInvalidSourceFileID};
    int line{0};
    int col{0};
};

struct TypedefEntry {
    std::string name;
    std::string resolved; // best-effort one-step type name
    std::string parent_scope; // package/class scope when known
    SourceFileID file_id{kInvalidSourceFileID};
    bool is_enum{false};
    bool is_struct{false};
    std::vector<EnumMemberEntry> enum_members;
    std::vector<FieldEntry> fields;
    int line{0};
    int col{0};
};

struct MacroEntry {
    std::string name;
    SourceFileID file_id{kInvalidSourceFileID};
    bool is_function_like{false};
    std::vector<std::string> params;
    int line{0};
};

struct ValueEntry {
    std::string name;
    std::string type;
    std::string kind; // variable/net/function/task/parameter/localparam/port
    std::string parent_scope; // module/interface/package/class name when known
    SourceFileID file_id{kInvalidSourceFileID};
    // 1-based lexical visibility range for block-local declarations.  A zero
    // range means the symbol is visible throughout parent_scope.
    int scope_start_line{0};
    int scope_end_line{0};
    int line{0};
    int col{0};
};

struct ImportEntry {
    std::string package_name;
    std::string symbol_name; // empty when wildcard == true
    bool wildcard{false};
    std::string parent_scope; // empty means compilation-unit import
    SourceFileID file_id{kInvalidSourceFileID};
    // 1-based visibility range. end_line == 0 means visible to end of file.
    int start_line{0};
    int end_line{0};
};

struct ReferenceEntry {
    std::string name;
    // Actual source file for this occurrence when the token came from another
    // file via `include`.
    //
    // A project index shard is keyed by the file that was parsed, but slang's
    // SyntaxTree may contain tokens from included headers:
    //
    //     a.sv        `include "defs.svh"
    //     defs.svh    task add_number; endtask
    //
    // If we report the shard URI (`a.sv`) for every occurrence, the line/column
    // from `defs.svh` points at unrelated text in `a.sv`.  Keep the actual URI
    // here as a compact FileID so reference results can jump to the real
    // header location. Invalid means "use the shard/open-document URI" for
    // older/synthesized entries.
    SourceFileID file_id{kInvalidSourceFileID};
    // Stable-enough syntactic semantic identity used by the project index.
    //
    // References in closed project files cannot be re-resolved through a live
    // AST on every request.  A plain name match is too broad:
    //
    //     module cpu(input clk); endmodule
    //     module uart(input clk); endmodule
    //
    // Searching references to cpu.clk must not return uart.clk just because the
    // spelling is the same.  `symbol_id` therefore stores the compact hash of
    // a resolved canonical owner path when parser context makes it available,
    // for example:
    //
    //     module:memory
    //     port:memory:clk
    //     param:memory:DEPTH
    //
    // Some identifiers are still not semantically resolved by the lightweight
    // index.  Those keep the conservative fallback form `name:<identifier>`.
    SymbolID symbol_id;
    // Debug / migration spelling for `symbol_id`.
    //
    // Keep this non-authoritative string while the codebase migrates away from
    // string IDs.  It makes tests and logs understandable and lets us recognize
    // unresolved fallback IDs (`name:<identifier>`) without reversing a hash.
    std::string symbol_debug;
    int line{0};    // 1-based, 0 if unknown
    int col{0};     // 0-based
    int end_col{0}; // 0-based exclusive
};

// ─────────────────────────────────────────────────────────────────────────────

enum class IndexDepth {
    Full,         ///< Everything — used for explicit full-index builds.
    Declarations, ///< Skips two expensive full-tree walks:
                  ///<   - LocalVariableVisitor (block-local vars inside
                  ///<     always/initial/function bodies)
                  ///<   - collect_imports (package import declarations)
                  ///< Still collects: modules, ports, instances, modports,
                  ///< module-level data/function members, classes, typedefs,
                  ///< package symbols, and macros.
                  ///< Used for .f extra-file states.
};

struct SyntaxIndex {
    // Per-index file table.  All source-backed entries store SourceFileID
    // instead of repeating URI strings.  merge() remaps IDs from shard-local
    // tables into the merged table.
    std::vector<std::string> source_files;

    // Modules (also used for interface/package declarations — see interface_names / package_names)
    std::vector<ModuleEntry>   modules;
    std::vector<InstanceEntry> instances;
    std::unordered_map<std::string, size_t> module_by_name;

    // Names that are interfaces (subset of module_by_name keys)
    std::unordered_set<std::string> interface_names;
    // Names that are packages (subset of module_by_name keys)
    std::unordered_set<std::string> package_names;
    // Package name → exported symbol names (functions, typedefs, params, etc.)
    std::unordered_map<std::string, std::vector<std::string>> package_symbols;

    // Classes
    std::vector<ClassEntry> classes;
    std::unordered_map<std::string, size_t> class_by_name;

    // Typedefs (including enum typedefs with their members)
    std::vector<TypedefEntry> typedefs;
    std::unordered_map<std::string, size_t> typedef_by_name;

    // Macros (`define entries)
    std::vector<MacroEntry> macros;

    // Values visible to completion: module/package variables, nets, parameters,
    // functions, tasks, and ports. This is a lightweight syntactic index; it is
    // not a full semantic symbol table, but it gives completion enough local
    // scope/type facts for common RTL editing.
    std::vector<ValueEntry> values;

    // Package imports visible to identifier completion.  These are syntactic
    // facts from PackageImportDeclarationSyntax, not preprocessor-expanded
    // semantic imports.  Completion uses them to avoid flattening package
    // libraries such as UVM into files that did not import them.
    std::vector<ImportEntry> imports;

    // Identifier occurrences for scalable cross-file references / rename.
    // These are syntactic occurrences, not a full semantic xref graph.  Current
    // and open files still validate through AST resolution; closed project files
    // use this compact occurrence index until a richer resolved-reference index
    // is available.
    std::vector<ReferenceEntry> references;

    /// Build index from a parsed SyntaxTree.
    /// @param source  the source text that produced @p tree (used for line-number lookup).
    static SyntaxIndex build(const slang::syntax::SyntaxTree& tree, std::string_view source = {},
                             IndexDepth depth = IndexDepth::Full);

    /// Merge all collections from @p other into this index.
    /// Used to combine extra-file indexes with the current document's index.
    void merge(const SyntaxIndex& other);

    SourceFileID intern_source_file(std::string uri);
    std::string source_uri(SourceFileID file_id) const;
};
