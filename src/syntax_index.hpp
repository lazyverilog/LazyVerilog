#pragma once
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace slang::syntax {
class SyntaxTree;
}

struct PortEntry {
    std::string name;
    std::string direction; // input/output/inout/ref
    std::string type;
    // Complete datatype / net declaration prefix for command/UI metadata. This
    // may include a net kind that `type` omits for display compatibility, for
    // example `wire [5:0]` for `input wire [5:0] a`.
    std::string decl_type;
    // Datatype to use when synthesizing an internal bridge signal. This is
    // derived from slang syntax facts, not from string inspection. Net ports
    // become variables, for example `output wire [5:0] o` -> `logic [5:0] s`.
    std::string signal_decl_type;
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

struct SyntaxIndex {
    std::vector<ModuleEntry> modules;
    std::vector<InstanceEntry> instances;
    std::unordered_map<std::string, size_t> module_by_name;

    /// Build index from a parsed SyntaxTree.
    /// @param source  the source text that produced @p tree (used for line-number lookup).
    static SyntaxIndex build(const slang::syntax::SyntaxTree& tree, std::string_view source = {});
};
