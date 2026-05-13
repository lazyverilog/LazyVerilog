#include "syntax_index.hpp"
#include <algorithm>
#include <cctype>
#include <slang/syntax/AllSyntax.h>
#include <slang/syntax/SyntaxTree.h>
#include <slang/text/SourceManager.h>
#include <string_view>

using namespace slang;
using namespace slang::syntax;

static std::string tok_str(const slang::parsing::Token& token) {
    return std::string(token.valueText());
}

static std::string trim_copy(std::string text) {
    auto first =
        std::find_if_not(text.begin(), text.end(), [](unsigned char c) { return std::isspace(c); });
    auto last = std::find_if_not(text.rbegin(), text.rend(), [](unsigned char c) {
                    return std::isspace(c);
                }).base();
    if (first >= last)
        return {};
    return std::string(first, last);
}

static std::pair<int, int> token_pos(const slang::SourceManager& sm,
                                     const slang::parsing::Token& token) {
    if (!token || !token.location().valid())
        return {0, 0};
    const auto line = sm.getLineNumber(token.location());
    const auto col = sm.getColumnNumber(token.location());
    return {line > 0 ? (int)line : 0, col > 0 ? (int)col - 1 : 0};
}

static std::string direction_of(const PortHeaderSyntax& header) {
    if (const auto* variable = header.as_if<VariablePortHeaderSyntax>())
        return tok_str(variable->direction).empty() ? "unknown" : tok_str(variable->direction);
    if (const auto* net = header.as_if<NetPortHeaderSyntax>())
        return tok_str(net->direction).empty() ? "unknown" : tok_str(net->direction);
    return "unknown";
}

static std::string type_of(const PortHeaderSyntax& header) {
    if (const auto* variable = header.as_if<VariablePortHeaderSyntax>())
        return trim_copy(variable->dataType->toString());
    if (const auto* net = header.as_if<NetPortHeaderSyntax>())
        return trim_copy(net->dataType->toString());
    return {};
}

static void add_port(std::vector<PortEntry>& ports, const slang::SourceManager& sm,
                     const slang::parsing::Token& name, std::string direction, std::string type) {
    if (!name)
        return;

    auto [line, col] = token_pos(sm, name);
    ports.push_back(PortEntry{
        .name = tok_str(name),
        .direction = std::move(direction),
        .type = std::move(type),
        .line = line,
        .col = col,
    });
}

static void extract_ansi_ports(const AnsiPortListSyntax& port_list, std::vector<PortEntry>& ports,
                               const slang::SourceManager& sm) {
    for (const auto* member : port_list.ports) {
        if (!member)
            continue;

        if (const auto* implicit = member->as_if<ImplicitAnsiPortSyntax>()) {
            add_port(ports, sm, implicit->declarator->name, direction_of(*implicit->header),
                     type_of(*implicit->header));
        } else if (const auto* explicit_port = member->as_if<ExplicitAnsiPortSyntax>()) {
            auto direction = tok_str(explicit_port->direction);
            add_port(ports, sm, explicit_port->name,
                     direction.empty() ? std::string("unknown") : std::move(direction), {});
        }
    }
}

static void extract_port_declarations(const SyntaxList<MemberSyntax>& members,
                                      std::vector<PortEntry>& ports,
                                      const slang::SourceManager& sm) {
    for (const auto* member : members) {
        if (!member)
            continue;
        const auto* declaration = member->as_if<PortDeclarationSyntax>();
        if (!declaration)
            continue;

        const auto direction = direction_of(*declaration->header);
        const auto type = type_of(*declaration->header);
        for (const auto* declarator : declaration->declarators) {
            if (declarator)
                add_port(ports, sm, declarator->name, direction, type);
        }
    }
}

static std::vector<std::string_view> split_lines(std::string_view source) {
    std::vector<std::string_view> lines;
    size_t start = 0;
    while (start <= source.size()) {
        size_t end = source.find('\n', start);
        if (end == std::string_view::npos) {
            lines.push_back(source.substr(start));
            break;
        }
        lines.push_back(source.substr(start, end - start));
        start = end + 1;
    }
    if (lines.empty())
        lines.push_back({});
    return lines;
}

static int find_instance_end_line(std::string_view source, int start_line) {
    auto lines = split_lines(source);
    if (start_line < 0)
        start_line = 0;
    for (int line = start_line; line < (int)lines.size(); ++line) {
        if (lines[line].find(';') != std::string_view::npos)
            return line;
    }
    return lines.empty() ? 0 : (int)lines.size() - 1;
}

static void extract_instances(const SyntaxList<MemberSyntax>& members,
                              std::vector<InstanceEntry>& out, const slang::SourceManager& sm,
                              std::string_view source) {
    for (const auto* member : members) {
        if (!member)
            continue;
        const auto* hierarchy = member->as_if<HierarchyInstantiationSyntax>();
        if (!hierarchy)
            continue;

        const std::string module_name = tok_str(hierarchy->type);
        for (const auto* instance : hierarchy->instances) {
            if (!instance)
                continue;

            InstanceEntry entry;
            entry.module_name = module_name;
            if (instance->decl) {
                entry.instance_name = tok_str(instance->decl->name);
                entry.line = token_pos(sm, instance->decl->name).first;
            }
            entry.start_line = entry.line > 0 ? entry.line - 1 : 0;
            entry.end_line = source.empty() ? entry.start_line
                                            : find_instance_end_line(source, entry.start_line);

            for (const auto* connection : instance->connections) {
                if (!connection)
                    continue;
                const auto* named = connection->as_if<NamedPortConnectionSyntax>();
                if (!named)
                    continue;

                auto [line, col] = token_pos(sm, named->name);
                auto [paren_line, paren_col] = token_pos(sm, named->openParen);
                entry.connections.push_back(NamedPortConn{
                    .port_name = tok_str(named->name),
                    .line = line,
                    .col = col,
                    .hint_col = paren_line == line ? paren_col + 1 : col,
                });
            }
            out.push_back(std::move(entry));
        }
    }
}

static void process_module(const ModuleDeclarationSyntax& module, SyntaxIndex& index,
                           const slang::SourceManager& sm, std::string_view source) {
    ModuleEntry entry;
    entry.name = tok_str(module.header->name);
    auto [line, col] = token_pos(sm, module.header->name);
    entry.line = line;
    entry.col = col;

    if (module.header->ports) {
        if (const auto* ansi = module.header->ports->as_if<AnsiPortListSyntax>())
            extract_ansi_ports(*ansi, entry.ports, sm);
    }
    extract_port_declarations(module.members, entry.ports, sm);
    extract_instances(module.members, index.instances, sm, source);
    index.modules.push_back(std::move(entry));
}

SyntaxIndex SyntaxIndex::build(const slang::syntax::SyntaxTree& tree, std::string_view source) {
    SyntaxIndex index;
    const auto& sm = tree.sourceManager();
    const auto& root = tree.root();

    if (const auto* compilation_unit = root.as_if<CompilationUnitSyntax>()) {
        for (const auto* member : compilation_unit->members) {
            if (!member)
                continue;
            if (const auto* module = member->as_if<ModuleDeclarationSyntax>())
                process_module(*module, index, sm, source);
        }
    } else if (const auto* module = root.as_if<ModuleDeclarationSyntax>()) {
        process_module(*module, index, sm, source);
    }

    return index;
}
