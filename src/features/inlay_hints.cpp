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


/// The modules this request can resolve an instance against, by name.
///
/// Entries are borrowed, not copied.  A ModuleEntry carries the module's whole
/// port list, each port a handful of std::strings, so copying one per module in
/// the design cost the request a deep copy of every port in the project -- once
/// per keystroke, since Neovim asks for hints on every didChange.  The owners
/// are kept alive alongside the pointers instead.
///
/// Only the names the edited file actually instantiates are resolved.  A file
/// instantiates a handful of module types; a project declares tens of thousands,
/// and populating the map from the whole project made the request scale with the
/// design rather than with the buffer -- measured 1.5 ms -> 2.9 ms per keystroke
/// going from 51 to 4001 project modules, on a file with no instances at all,
/// while foldingRange over the same two projects stayed flat.  The project
/// snapshot already indexes modules by name, so asking it for the few names that
/// matter is a hash lookup each instead of a rebuild of the whole table.
struct ModuleMap {
    std::unordered_map<std::string, const ModuleEntry*> by_name;
    // What the pointers point into.  The project snapshot is immutable once
    // published and each DocumentState is an immutable snapshot, so holding a
    // reference to them is all it takes to keep every borrowed entry valid for
    // the life of this request.
    std::shared_ptr<const ProjectIndexSnapshot> project;
    std::vector<std::shared_ptr<const DocumentState>> open_documents;
};

/// Resolve @p wanted against the open buffers and then the project snapshot.
///
/// Open buffers win, and are applied first for that reason: they carry unsaved
/// edits, so a module declared in a buffer must shadow the shard published for
/// the same name.  Populating project-first and overlaying the buffers on top
/// gave the same precedence by overwriting, which is only affordable when the
/// project half is cheap.
static ModuleMap build_module_map(const Analyzer& analyzer,
                                  const std::unordered_set<std::string>& wanted) {
    ModuleMap modules;
    if (wanted.empty())
        return modules;

    analyzer.for_each_state(
        [&](const std::string&, const std::shared_ptr<const DocumentState>& state) {
            if (!state)
                return;
            // A buffer whose reparse is in flight is a text-only placeholder.
            // Skipping it dropped every module it declares from the map for as
            // long as that parse ran, so an instance of a module declared in
            // the file being typed in lost its hints even though the instance
            // side had a tree to answer from -- the same blink the
            // get_parsed_state() call below exists to prevent, arriving by the
            // other half of the lookup.  One keystroke stale is the right
            // answer here for the same reason it is there.
            const auto& usable = state->tree ? state : state->previous_parsed;
            if (!usable || !usable->tree)
                return;
            const auto& index = get_structural_index(*usable);
            bool borrowed = false;
            for (const auto& module : index.modules) {
                if (!wanted.contains(module.name))
                    continue;
                modules.by_name[module.name] = &module;
                borrowed = true;
            }
            // The structural index lives on the snapshot, so the snapshot has
            // to outlive the pointers taken from it -- but only when one was
            // actually taken.
            if (borrowed)
                modules.open_documents.push_back(usable);
        });

    auto project = analyzer.project_index_snapshot();
    if (!project)
        return modules;
    bool borrowed = false;
    for (const auto& name : wanted) {
        if (modules.by_name.contains(name))
            continue; // an open buffer already answered for this name
        const auto it = project->module_by_name.find(name);
        if (it == project->module_by_name.end())
            continue;
        const auto& ref = it->second;
        if (!ref.shard || ref.module_index >= ref.shard->modules.size())
            continue;
        modules.by_name[name] = &ref.shard->modules[ref.module_index];
        borrowed = true;
    }
    if (borrowed)
        modules.project = std::move(project);
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

    // By reference: get_structural_index() hands back the index cached on the
    // snapshot, and binding it to a value copied every declaration, instance
    // and reference occurrence in the file on every request.
    const auto& current_index = get_structural_index(*state);
    // Every hint this request can emit hangs off an instance, so a file with
    // none has nothing to answer and nothing to look anything up for.  Leaf RTL
    // is mostly this shape, and it is asked on every keystroke like the rest.
    if (current_index.instances.empty())
        return {};

    std::unordered_set<std::string> instantiated;
    for (const auto& inst : current_index.instances)
        instantiated.insert(inst.module_name);

    const auto lines = split_lines_view(state->text);
    const auto modules = build_module_map(analyzer, instantiated);
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
