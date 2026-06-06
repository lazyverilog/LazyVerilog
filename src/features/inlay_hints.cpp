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


using ModuleMap = std::unordered_map<std::string, ModuleEntry>;

static void overlay_modules(ModuleMap& modules, const SyntaxIndex& index) {
    for (const auto& module : index.modules)
        modules[module.name] = module;
}

static ModuleMap build_module_map(const Analyzer& analyzer) {
    ModuleMap modules;

    if (auto project_index = analyzer.project_index_snapshot()) {
        for (const auto& [name, ref] : project_index->module_by_name) {
            if (ref.shard && ref.module_index < ref.shard->modules.size())
                modules[name] = ref.shard->modules[ref.module_index];
        }
    }

    analyzer.for_each_state(
        [&](const std::string&, const std::shared_ptr<const DocumentState>& state) {
            if (state && state->tree)
                overlay_modules(modules, get_structural_index(*state));
        });

    return modules;
}

static std::unordered_map<std::string, PortEntry> build_port_map(const ModuleEntry& module) {
    std::unordered_map<std::string, PortEntry> ports;
    for (const auto& port : module.ports)
        ports[port.name] = port;
    return ports;
}

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
    auto state = analyzer.get_state(uri);
    if (!state || !state->tree)
        return {};

    const auto lines = split_lines_view(state->text);
    const auto current_index = get_structural_index(*state);
    const auto modules = build_module_map(analyzer);
    std::vector<lsInlayHint> hints;

    for (const auto& inst : current_index.instances) {
        auto module_it = modules.find(inst.module_name);
        if (module_it == modules.end())
            continue;

        const auto port_map = build_port_map(module_it->second);
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
