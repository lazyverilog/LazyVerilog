#include "inlay_hints.hpp"

#include "syntax_index.hpp"

#include <algorithm>
#include <optional>
#include <slang/syntax/SyntaxTree.h>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace {

static std::vector<std::string_view> split_lines(std::string_view text) {
    std::vector<std::string_view> lines;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t end = text.find('\n', start);
        if (end == std::string_view::npos) {
            lines.push_back(text.substr(start));
            break;
        }
        lines.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    if (lines.empty())
        lines.push_back({});
    return lines;
}

using ModuleMap = std::unordered_map<std::string, ModuleEntry>;

static void overlay_modules(ModuleMap& modules, const SyntaxIndex& index) {
    for (const auto& module : index.modules)
        modules[module.name] = module;
}

static ModuleMap build_module_map(const Analyzer& analyzer) {
    ModuleMap modules;

    for (const auto& extra : analyzer.extra_file_snapshots())
        overlay_modules(modules, extra.index);

    analyzer.for_each_state(
        [&](const std::string&, const std::shared_ptr<const DocumentState>& state) {
            if (state && state->tree)
                overlay_modules(modules, SyntaxIndex::build(*state->tree, state->text));
        });

    return modules;
}

static std::unordered_map<std::string, PortEntry> build_port_map(const ModuleEntry& module) {
    std::unordered_map<std::string, PortEntry> ports;
    for (const auto& port : module.ports)
        ports[port.name] = port;
    return ports;
}

static std::string padded(std::string text, size_t width) {
    if (text.size() < width)
        text.append(width - text.size(), ' ');
    return text;
}

} // namespace

std::vector<lsInlayHint> provide_inlay_hints(const Analyzer& analyzer, const std::string& uri,
                                             int range_start_line, int range_end_line) {
    auto state = analyzer.get_state(uri);
    if (!state || !state->tree)
        return {};

    const auto lines = split_lines(state->text);
    const auto current_index = SyntaxIndex::build(*state->tree, state->text);
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
            std::string type;
        };
        std::vector<Candidate> candidates;
        std::unordered_set<std::string> connected;

        for (const auto& conn : inst.connections) {
            const int line = conn.line > 0 ? conn.line - 1 : 0;
            connected.insert(conn.port_name);

            if (line < lo || line >= hi)
                continue;

            auto port_it = port_map.find(conn.port_name);
            if (port_it == port_map.end())
                continue;

            auto direction =
                port_it->second.direction == "unknown" ? std::string{} : port_it->second.direction;
            candidates.push_back(Candidate{
                .line = line,
                .col = conn.hint_col,
                .direction = std::move(direction),
                .type = port_it->second.type,
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

        size_t max_dir = 0;
        size_t max_type = 0;
        int max_col = 0;
        for (const auto& candidate : candidates) {
            max_dir = std::max(max_dir, candidate.direction.size());
            max_type = std::max(max_type, candidate.type.size());
            max_col = std::max(max_col, candidate.col);
        }

        std::vector<std::pair<int, std::string>> labels;
        size_t max_label = 0;
        for (const auto& candidate : candidates) {
            std::vector<std::string> parts;
            auto dir = padded(candidate.direction, max_dir);
            if (dir.find_first_not_of(' ') != std::string::npos)
                parts.push_back(std::move(dir));

            if (max_type > 0) {
                auto type = padded(candidate.type, max_type);
                if (type.find_first_not_of(' ') != std::string::npos)
                    parts.push_back(std::move(type));
            }

            std::string label;
            for (size_t i = 0; i < parts.size(); ++i) {
                if (i > 0)
                    label += ' ';
                label += parts[i];
            }

            if (label.find_first_not_of(' ') == std::string::npos)
                continue;

            max_label = std::max(max_label, label.size());
            labels.emplace_back(candidate.line, std::move(label));
        }

        for (auto& [line, label] : labels) {
            lsInlayHint hint;
            hint.position = lsPosition(line, max_col);
            hint.label = padded(std::move(label), max_label);
            hint.kind = optional<lsInlayHintKind>(lsInlayHintKind::Type);
            hint.paddingLeft = optional<bool>(false);
            hint.paddingRight = optional<bool>(true);
            hints.push_back(std::move(hint));
        }
    }

    return hints;
}
