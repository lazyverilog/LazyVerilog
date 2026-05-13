#pragma once
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace slang::syntax {
class SyntaxTree;
}

struct PortEntry {
    std::string name;
    std::string direction; // input/output/inout/ref
    std::string type;
    int line{0}; // 1-based, 0 if unknown
    int col{0};  // 0-based
};

struct ModuleEntry {
    std::string name;
    int line{0}; // 1-based, 0 if unknown
    int col{0};  // 0-based
    std::vector<PortEntry> ports;
};

struct NamedPortConn {
    std::string port_name;
    int line{0};
    int col{0};
    int hint_col{0};
};

struct InstanceEntry {
    std::string module_name;
    std::string instance_name;
    int line{0};
    int start_line{0}; // 0-based
    int end_line{0};   // 0-based
    std::vector<NamedPortConn> connections;
};

struct SyntaxIndex {
    std::vector<ModuleEntry> modules;
    std::vector<InstanceEntry> instances;

    /// Build index from a parsed SyntaxTree.
    /// @param source  the source text that produced @p tree (used for line-number lookup).
    static SyntaxIndex build(const slang::syntax::SyntaxTree& tree, std::string_view source = {});
};
