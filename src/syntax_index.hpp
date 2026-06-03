#pragma once
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace slang::syntax {
class SyntaxTree;
}

struct PortEntry {
    std::string name;
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

struct ModuleEntry {
    std::string name;
    int line{0}; // 1-based, 0 if unknown
    int col{0};  // 0-based
    std::vector<PortEntry> ports;
    std::unordered_map<std::string, size_t> port_by_name;
};

struct NamedPortConn {
    std::string port_name;
    std::string signal_name;
    int line{0};
    int col{0};
    int hint_col{0};
};

struct InstanceEntry {
    std::string module_name;
    std::string instance_name;
    std::string parent_module;
    int line{0};
    int start_line{0}; // 0-based
    int end_line{0};   // 0-based
    std::vector<NamedPortConn> connections;
};

// ── New symbol types for completion ───────────────────────────────────────────

struct FieldEntry {
    std::string name;
    std::string type; // syntactic type text (best-effort)
    int line{0};
    int col{0};
};

struct MethodEntry {
    std::string name;
    std::string return_type;
    bool is_task{false};
    int line{0};
    int col{0};
};

struct ClassEntry {
    std::string name;
    std::string base_class; // empty if no extends clause
    std::vector<FieldEntry> fields;
    std::vector<MethodEntry> methods;
    int line{0};
    int col{0};
};

struct EnumMemberEntry {
    std::string name;
};

struct TypedefEntry {
    std::string name;
    std::string resolved; // best-effort one-step type name
    bool is_enum{false};
    bool is_struct{false};
    std::vector<EnumMemberEntry> enum_members;
    std::vector<FieldEntry> fields;
    int line{0};
};

struct MacroEntry {
    std::string name;
    bool is_function_like{false};
    std::vector<std::string> params;
    int line{0};
};

struct ValueEntry {
    std::string name;
    std::string type;
    std::string kind; // variable/net/function/task/parameter/localparam/port
    std::string parent_scope; // module/interface/package/class name when known
    // 1-based lexical visibility range for block-local declarations.  A zero
    // range means the symbol is visible throughout parent_scope.
    int scope_start_line{0};
    int scope_end_line{0};
    int line{0};
    int col{0};
};

// ─────────────────────────────────────────────────────────────────────────────

struct SyntaxIndex {
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

    /// Build index from a parsed SyntaxTree.
    /// @param source  the source text that produced @p tree (used for line-number lookup).
    static SyntaxIndex build(const slang::syntax::SyntaxTree& tree, std::string_view source = {});

    /// Merge all collections from @p other into this index.
    /// Used to combine extra-file indexes with the current document's index.
    void merge(const SyntaxIndex& other);
};
