#include "inlay_hints.hpp"

#include "dynamic_file_index.hpp"
#include "syntax_index.hpp"
#include "../string_utils.hpp"

#include <algorithm>
#include <optional>
#include <slang/syntax/SyntaxTree.h>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace {


/// Every module this request can resolve an instance against, by name.
///
/// Entries are borrowed, not copied.  A ModuleEntry carries the module's whole
/// port list, each port a handful of std::strings, so copying one per module in
/// the design cost the request a deep copy of every port in the project -- once
/// per keystroke, since Neovim asks for hints on every didChange.  The owners
/// are kept alive alongside the pointers instead.
struct ModuleMap {
    std::unordered_map<std::string, const ModuleEntry*> by_name;
    // What the pointers point into.  The project snapshot is immutable once
    // published and each DocumentState is an immutable snapshot, so holding a
    // reference to them is all it takes to keep every borrowed entry valid for
    // the life of this request.
    std::shared_ptr<const ProjectIndexSnapshot> project;
    std::vector<std::shared_ptr<const DocumentState>> open_documents;
};

static void overlay_modules(ModuleMap& modules, const SyntaxIndex& index) {
    for (const auto& module : index.modules)
        modules.by_name[module.name] = &module;
}

static ModuleMap build_module_map(const Analyzer& analyzer) {
    ModuleMap modules;

    modules.project = analyzer.project_index_snapshot();
    if (modules.project) {
        for (const auto& [name, ref] : modules.project->module_by_name) {
            if (ref.shard && ref.module_index < ref.shard->modules.size())
                modules.by_name[name] = &ref.shard->modules[ref.module_index];
        }
    }

    analyzer.for_each_state(
        [&](const std::string&, const std::shared_ptr<const DocumentState>& state) {
            if (!state || !state->tree)
                return;
            // The structural index lives on the snapshot, so the snapshot has
            // to outlive the pointers taken from it.
            modules.open_documents.push_back(state);
            overlay_modules(modules, get_structural_index(*state));
        });

    return modules;
}

using PortMap = std::unordered_map<std::string, PortEntry>;

static PortMap build_port_map(const ModuleEntry& module) {
    PortMap ports;
    for (const auto& port : module.ports) {
        // module.ports also holds `#(...)` header parameters (direction
        // "parameter"/"localparam"); those aren't instance port connections
        // and must not count toward the port total shown in the hint.
        if (port.direction == "parameter" || port.direction == "localparam")
            continue;
        ports[port.name] = port;
    }
    return ports;
}

/// Port maps built at most once per module for the life of one request.
///
/// A design instantiates the same few leaf modules over and over -- 600
/// instances of one module is an ordinary generated netlist -- and the port map
/// depends only on the module, so building it per instance did the same work
/// 600 times.
class PortMapCache {
  public:
    const PortMap& get(const std::string& module_name, const ModuleEntry& module) {
        const auto it = cache_.find(module_name);
        if (it != cache_.end())
            return it->second;
        return cache_.emplace(module_name, build_port_map(module)).first->second;
    }

  private:
    std::unordered_map<std::string, PortMap> cache_;
};

static std::string display_port_direction(const std::string& direction) {
    // Keep the semantic direction stored in SyntaxIndex unchanged
    // (`input`/`output`/`inout`/`unknown`) and translate only the inlay-hint
    // presentation label here.  Port inlay hints intentionally show direction
    // only; type/range text belongs in hover/definition where it does not add
    // inline visual noise.  Every visible direction label is a single glyph:
    //
    //   .req ◀ (req)
    //   .ack ▶ (ack)
    //   .bus ↔ (bus)
    //
    if (direction == "input")
        return "◀";
    if (direction == "output")
        return "▶";
    if (direction == "inout")
        return "↔";
    if (direction == "unknown")
        return "?";
    return {};
}

} // namespace

std::vector<lsInlayHint> provide_inlay_hints(const Analyzer& analyzer, const std::string& uri,
                                             int range_start_line, int range_end_line) {
    // Not get_state(): Neovim issues this request from the didChange
    // notification itself, so it lands while the parse that notification
    // started is still running and the current snapshot is text-only.  Giving
    // up there answered *every* request made during typing with nothing, and
    // the client renders that -- hints measurably blinked out for the whole
    // duration of a burst on a large file and came back only when an unrelated
    // project-index publish happened to fire a refresh.
    auto state = analyzer.get_parsed_state(uri);
    if (!state || !state->tree)
        return {};

    const auto lines = split_lines_view(state->text);
    // By reference: get_structural_index() hands back the index cached on the
    // snapshot, and binding it to a value copied every declaration, instance
    // and reference occurrence in the file on every request.
    const auto& current_index = get_structural_index(*state);
    const auto modules = build_module_map(analyzer);
    PortMapCache port_maps;
    std::vector<lsInlayHint> hints;

    for (const auto& inst : current_index.instances) {
        auto module_it = modules.by_name.find(inst.module_name);
        if (module_it == modules.by_name.end() || !module_it->second)
            continue;

        const auto& port_map = port_maps.get(inst.module_name, *module_it->second);
        if (port_map.empty())
            continue;

        const int lo = std::max(inst.start_line, range_start_line);
        const int hi = std::min(inst.end_line + 1, range_end_line + 1);

        struct Candidate {
            int line{0};
            int col{0};
            std::string direction;
        };
        std::vector<Candidate> candidates;
        std::unordered_set<std::string> connected;

        for (const auto& conn : inst.connections) {
            const int line = conn.line > 0 ? conn.line - 1 : 0;
            connected.insert(conn.port_name);

            if (line < lo || line >= hi)
                continue;

            auto port_it = port_map.find(conn.port_name);
            const std::string direction = port_it == port_map.end()
                                              ? std::string("?")
                                              : display_port_direction(port_it->second.direction);

            candidates.push_back(Candidate{
                .line = line,
                // Place the inlay hint immediately before the named port token,
                // which means between the dot and the port name:
                //
                //   .◀i_clk    (clk)
                //
                // Slang/index data gives us:
                //   conn.col      -> the first character of the port name in
                //                    `.port_name(...)`; this is exactly the
                //                    insertion point after the dot.
                //   conn.hint_col -> the first character of the connected
                //                    expression inside the parentheses.
                //
                // Keep conn.hint_col unchanged for connection-edit features.
                .col = conn.col,
                // A missing port means the instance connection is stale/extra
                // relative to the resolved module declaration.  Render the
                // same single-glyph unknown marker used for known ports whose
                // direction cannot be determined, instead of silently hiding
                // the connection.
                .direction = direction,
            });
        }

        if (candidates.empty() && connected.empty())
            continue;

        if (inst.start_line >= range_start_line && inst.start_line <= range_end_line &&
            inst.start_line < (int)lines.size()) {
            size_t connected_count = 0;
            for (const auto& name : connected) {
                if (port_map.contains(name))
                    ++connected_count;
            }

            lsInlayHint coverage;
            coverage.position = lsPosition(inst.start_line, (int)lines[inst.start_line].size());
            coverage.label =
                std::to_string(connected_count) + "/" + std::to_string(port_map.size()) + " ports";
            coverage.kind = optional<lsInlayHintKind>(lsInlayHintKind::Parameter);
            coverage.paddingLeft = optional<bool>(true);
            coverage.paddingRight = optional<bool>(false);
            hints.push_back(std::move(coverage));
        }

        if (candidates.empty())
            continue;

        struct Label {
            int line{0};
            int col{0};
            std::string text;
        };
        std::vector<Label> labels;
        for (const auto& candidate : candidates) {
            if (candidate.direction.empty())
                continue;

            labels.push_back(Label{
                .line = candidate.line,
                .col = candidate.col,
                .text = candidate.direction,
            });
        }

        for (auto& label : labels) {
            lsInlayHint hint;
            hint.position = lsPosition(label.line, label.col);
            // Do not re-pad labels to a shared width.  Older code did this to
            // make labels visually column-like, but those spaces become part of
            // the LSP label and show up as unwanted trailing padding after
            // direction-only hints such as "◀".
            hint.label = std::move(label.text);
            hint.kind = optional<lsInlayHintKind>(lsInlayHintKind::Type);
            hint.paddingLeft = optional<bool>(false);
            // The desired rendering is `.◀i_clk`, not `. ◀ i_clk`; disable
            // client-side padding on both sides of the single-glyph label.
            hint.paddingRight = optional<bool>(false);
            hints.push_back(std::move(hint));
        }
    }

    return hints;
}
