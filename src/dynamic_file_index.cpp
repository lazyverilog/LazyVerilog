#include "dynamic_file_index.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <optional>
#include <slang/parsing/TokenKind.h>
#include <slang/syntax/AllSyntax.h>
#include <slang/syntax/SyntaxTree.h>
#include <slang/syntax/SyntaxVisitor.h>
#include <slang/text/SourceManager.h>
#include <unordered_map>
#include <unordered_set>

using namespace slang;
using namespace slang::syntax;

namespace {

std::string tok_text(const slang::parsing::Token& token) {
    return token ? std::string(token.valueText()) : std::string{};
}

std::string trim_text(std::string text) {
    auto first =
        std::find_if_not(text.begin(), text.end(), [](unsigned char c) { return std::isspace(c); });
    auto last = std::find_if_not(text.rbegin(), text.rend(), [](unsigned char c) {
                    return std::isspace(c);
                }).base();
    if (first >= last)
        return {};
    return std::string(first, last);
}

std::string node_text(const SyntaxNode& node) {
    return trim_text(node.toString());
}

std::string node_text_raw(const slang::SourceManager& sm, const SyntaxNode& node) {
    const auto range = node.sourceRange();
    if (range.start().valid() && range.end().valid() &&
        range.start().buffer() == range.end().buffer()) {
        const auto source = sm.getSourceText(range.start().buffer());
        const size_t begin = range.start().offset();
        const size_t end = range.end().offset();
        if (begin <= end && end <= source.size())
            return trim_text(std::string(source.substr(begin, end - begin)));
    }
    return node_text(node);
}

std::pair<int, int> token_pos(const slang::SourceManager& sm,
                              const slang::parsing::Token& token) {
    if (!token || !token.location().valid())
        return {0, 0};
    const auto line = sm.getLineNumber(token.location());
    const auto col = sm.getColumnNumber(token.location());
    return {line > 0 ? (int)line : 0, col > 0 ? (int)col - 1 : 0};
}

std::pair<int, int> token_pos0(const slang::SourceManager& sm,
                               const slang::parsing::Token& token) {
    auto [line, col] = token_pos(sm, token);
    return {line > 0 ? line - 1 : -1, col};
}

std::string path_to_uri_for_dynamic_index(const std::filesystem::path& path) {
    return "file://" + std::filesystem::absolute(path).lexically_normal().string();
}

std::string token_uri_for_dynamic_index(const slang::SourceManager& sm,
                                        const slang::parsing::Token& token) {
    if (!token || !token.location().valid())
        return {};

    const auto file_name = sm.getFileName(token.location());
    if (file_name.empty())
        return {};

    std::string file(file_name);
    if (file.starts_with("file://"))
        return file;
    return path_to_uri_for_dynamic_index(file);
}

SourceFileID token_file_id_for_dynamic_index(SyntaxIndex& index, const slang::SourceManager& sm,
                                             const slang::parsing::Token& token) {
    auto uri = token_uri_for_dynamic_index(sm, token);
    if (uri.empty())
        return kInvalidSourceFileID;
    return index.intern_source_file(std::move(uri));
}

std::string location_uri_for_dynamic_index(const slang::SourceManager& sm,
                                           slang::SourceLocation location) {
    if (!location.valid())
        return {};

    const auto file_name = sm.getFileName(location);
    if (file_name.empty())
        return {};

    std::string file(file_name);
    if (file.starts_with("file://"))
        return file;
    return path_to_uri_for_dynamic_index(file);
}

SourceFileID location_file_id_for_dynamic_index(SyntaxIndex& index, const slang::SourceManager& sm,
                                                slang::SourceLocation location) {
    auto uri = location_uri_for_dynamic_index(sm, location);
    if (uri.empty())
        return kInvalidSourceFileID;
    return index.intern_source_file(std::move(uri));
}

void fill_module_edit_ranges(ModuleEntry& module, const ModuleHeaderSyntax& header,
                             const slang::SourceManager& sm) {
    auto [semi_line, semi_col] = token_pos0(sm, header.semi);
    module.header_semi_line = semi_line;
    module.header_semi_col = semi_col;

    if (!header.ports)
        return;

    module.has_port_list = true;
    module.ansi_port_list = header.ports->kind == SyntaxKind::AnsiPortList;

    slang::parsing::Token close;
    if (const auto* ansi = header.ports->as_if<AnsiPortListSyntax>()) {
        module.port_list_has_ports = !ansi->ports.empty();
        close = ansi->closeParen;
    } else if (const auto* non_ansi = header.ports->as_if<NonAnsiPortListSyntax>()) {
        module.port_list_has_ports = !non_ansi->ports.empty();
        close = non_ansi->closeParen;
    } else if (const auto* wildcard = header.ports->as_if<WildcardPortListSyntax>()) {
        module.port_list_has_ports = true;
        close = wildcard->closeParen;
    } else {
        module.port_list_has_ports = true;
        close = header.ports->getLastToken();
    }

    auto [close_line, close_col] = token_pos0(sm, close);
    module.port_list_close_line = close_line;
    module.port_list_close_col = close_col;
}

std::string symbol_canonical(std::string kind, std::string scope, std::string name) {
    if (scope.empty())
        return kind + "::" + name;
    return kind + "::" + scope + "::" + name;
}

bool is_module_value_kind(std::string_view kind) {
    return kind == "variable" || kind == "net" || kind == "parameter" || kind == "localparam" ||
           kind == "port";
}

std::string canonical_type_name_from_text(std::string_view type) {
    size_t end = type.size();
    while (end > 0 &&
           !(std::isalnum(static_cast<unsigned char>(type[end - 1])) || type[end - 1] == '_' ||
             type[end - 1] == '$'))
        --end;
    size_t begin = end;
    while (begin > 0 &&
           (std::isalnum(static_cast<unsigned char>(type[begin - 1])) ||
            type[begin - 1] == '_' || type[begin - 1] == '$'))
        --begin;
    return std::string(type.substr(begin, end - begin));
}

std::string simple_identifier_from_expr_node(const ExpressionSyntax* expr) {
    if (!expr)
        return {};
    if (const auto* ident = expr->as_if<IdentifierNameSyntax>())
        return tok_text(ident->identifier);
    return {};
}

std::string with_dims(const slang::SourceManager& sm, std::string type,
                      const DeclaratorSyntax& declarator) {
    for (const auto* dim : declarator.dimensions) {
        if (!dim)
            continue;
        auto d = node_text_raw(sm, *dim);
        if (!d.empty())
            type += (type.empty() ? "" : " ") + d;
    }
    return type;
}

ValueEntry* add_value(SyntaxIndex& index, const slang::SourceManager& sm,
                      const slang::parsing::Token& name, std::string type, std::string kind,
                      std::string parent_scope) {
    if (!name)
        return nullptr;
    auto [line, col] = token_pos(sm, name);
    index.values.push_back(ValueEntry{.name = tok_text(name),
                                      .type = std::move(type),
                                      .kind = std::move(kind),
                                      .parent_scope = std::move(parent_scope),
                                      .file_id = token_file_id_for_dynamic_index(index, sm, name),
                                      .line = line,
                                      .col = col});
    return &index.values.back();
}

void add_port(ModuleEntry& module, SyntaxIndex& index, const slang::SourceManager& sm,
              const slang::parsing::Token& name, std::string direction, std::string type,
              std::string decl_type = {}, std::string signal_decl_type = {},
              std::string default_value = {}) {
    if (!name)
        return;
    auto [line, col] = token_pos(sm, name);
    module.ports.push_back(PortEntry{.name = tok_text(name),
                                     .file_id = token_file_id_for_dynamic_index(index, sm, name),
                                     .direction = direction,
                                     .type = type,
                                     .decl_type = decl_type.empty() ? type : std::move(decl_type),
                                     .signal_decl_type =
                                         signal_decl_type.empty() ? type : std::move(signal_decl_type),
                                     .default_value = std::move(default_value),
                                     .line = line,
                                     .col = col});
    index.values.push_back(ValueEntry{.name = tok_text(name),
                                      .type = std::move(type),
                                      .kind = (direction == "parameter" || direction == "localparam")
                                                  ? direction
                                                  : std::string("port"),
                                      .parent_scope = module.name,
                                      .file_id = token_file_id_for_dynamic_index(index, sm, name),
                                      .line = line,
                                      .col = col});
}

std::string port_direction(const PortHeaderSyntax& header) {
    if (const auto* variable = header.as_if<VariablePortHeaderSyntax>())
        return tok_text(variable->direction).empty() ? "unknown" : tok_text(variable->direction);
    if (const auto* net = header.as_if<NetPortHeaderSyntax>())
        return tok_text(net->direction).empty() ? "unknown" : tok_text(net->direction);
    return "unknown";
}

std::string port_type(const slang::SourceManager& sm, const PortHeaderSyntax& header) {
    if (const auto* variable = header.as_if<VariablePortHeaderSyntax>())
        return node_text_raw(sm, *variable->dataType);
    if (const auto* net = header.as_if<NetPortHeaderSyntax>())
        return node_text_raw(sm, *net->dataType);
    return {};
}

std::string port_decl_type(const slang::SourceManager& sm, const PortHeaderSyntax& header) {
    if (const auto* variable = header.as_if<VariablePortHeaderSyntax>())
        return node_text_raw(sm, *variable->dataType);
    if (const auto* net = header.as_if<NetPortHeaderSyntax>()) {
        std::string text = tok_text(net->netType);
        const auto data = node_text_raw(sm, *net->dataType);
        if (!data.empty())
            text += (text.empty() ? "" : " ") + data;
        return text;
    }
    return {};
}

std::string port_signal_decl_type(const slang::SourceManager& sm, const PortHeaderSyntax& header) {
    if (const auto* variable = header.as_if<VariablePortHeaderSyntax>())
        return node_text_raw(sm, *variable->dataType);
    if (const auto* net = header.as_if<NetPortHeaderSyntax>()) {
        std::string text = "logic";
        const auto data = node_text_raw(sm, *net->dataType);
        if (!data.empty())
            text += " " + data;
        return text;
    }
    return {};
}

std::string simple_identifier_from_expr(const PropertyExprSyntax* expr) {
    if (!expr)
        return {};
    if (const auto* prop = expr->as_if<SimplePropertyExprSyntax>()) {
        if (const auto* seq = prop->expr->as_if<SimpleSequenceExprSyntax>()) {
            if (const auto* ident = seq->expr->as_if<IdentifierNameSyntax>())
                return tok_text(ident->identifier);
        }
    }
    return {};
}

std::vector<std::string_view> split_lines(std::string_view source) {
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
    return lines;
}

int instance_end_line(const std::vector<std::string_view>& lines, int start_line) {
    for (int i = start_line; i < (int)lines.size(); ++i) {
        if (lines[(size_t)i].find(';') != std::string_view::npos)
            return i;
    }
    return start_line;
}

int node_end_line0(const slang::SourceManager& sm, const SyntaxNode& node, int fallback_line) {
    const auto end = node.sourceRange().end();
    if (!end.valid())
        return fallback_line;
    const auto line = sm.getLineNumber(end);
    return line > 0 ? static_cast<int>(line) - 1 : fallback_line;
}

void process_hierarchy(const HierarchyInstantiationSyntax& hierarchy, SyntaxIndex& index,
                       const slang::SourceManager& sm, const std::vector<std::string_view>& lines,
                       const std::string& parent_module) {
    const std::string module_name = tok_text(hierarchy.type);
    for (const auto* inst : hierarchy.instances) {
        if (!inst)
            continue;
        InstanceEntry entry;
        entry.module_name = module_name;
        entry.parent_module = parent_module;
        if (inst->decl) {
            entry.instance_name = tok_text(inst->decl->name);
            entry.file_id = token_file_id_for_dynamic_index(index, sm, inst->decl->name);
            entry.line = token_pos(sm, inst->decl->name).first;
        }
        entry.start_line = entry.line > 0 ? entry.line - 1 : 0;
        // Prefer slang's parsed source range over a raw ';' line scan.  The
        // scanner remains only as a fallback for malformed/incomplete syntax
        // where the parser cannot provide a valid range; raw text can be
        // fooled by semicolons inside strings or comments.
        entry.end_line = node_end_line0(
            sm, hierarchy,
            lines.empty() ? entry.start_line : instance_end_line(lines, entry.start_line));
        for (const auto* conn : inst->connections) {
            if (const auto* named = conn ? conn->as_if<NamedPortConnectionSyntax>() : nullptr) {
                auto [line, col] = token_pos(sm, named->name);
                auto [paren_line, paren_col] = token_pos(sm, named->openParen);
                entry.connections.push_back(NamedPortConn{.port_name = tok_text(named->name),
                                                          .signal_name =
                                                              simple_identifier_from_expr(named->expr),
                                                          .file_id = token_file_id_for_dynamic_index(index, sm, named->name),
                                                          .line = line,
                                                          .col = col,
                                                          .hint_col = paren_line == line
                                                                          ? paren_col + 1
                                                                          : col});
            }
        }
        index.instances.push_back(std::move(entry));
    }
}

void process_class(const ClassDeclarationSyntax& cls, SyntaxIndex& index,
                   const slang::SourceManager& sm, std::string parent_scope = {}) {
    ClassEntry entry;
    entry.name = tok_text(cls.name);
    entry.file_id = token_file_id_for_dynamic_index(index, sm, cls.name);
    entry.parent_scope = std::move(parent_scope);
    auto [line, col] = token_pos(sm, cls.name);
    entry.line = line;
    entry.col = col;
    if (cls.extendsClause)
        entry.base_class = node_text(*cls.extendsClause->baseName);

    for (const auto* item : cls.items) {
        if (!item)
            continue;
        if (const auto* prop = item->as_if<ClassPropertyDeclarationSyntax>()) {
            if (const auto* data = prop->declaration->as_if<DataDeclarationSyntax>()) {
                const auto type = node_text(*data->type);
                for (const auto* decl : data->declarators) {
                    if (!decl)
                        continue;
                    auto [fl, fc] = token_pos(sm, decl->name);
                    entry.fields.push_back(FieldEntry{.name = tok_text(decl->name),
                                                      .type = with_dims(sm, type, *decl),
                                                      .file_id = token_file_id_for_dynamic_index(index, sm, decl->name),
                                                      .line = fl,
                                                      .col = fc});
                }
            }
        } else if (const auto* method = item->as_if<ClassMethodDeclarationSyntax>()) {
            const auto& proto = *method->declaration->prototype;
            auto [ml, mc] = token_pos(sm, proto.keyword);
            entry.methods.push_back(MethodEntry{.name = node_text(*proto.name),
                                                .return_type = node_text(*proto.returnType),
                                                .is_task = method->declaration->kind ==
                                                           SyntaxKind::TaskDeclaration,
                                                .file_id = token_file_id_for_dynamic_index(index, sm, proto.keyword),
                                                .line = ml,
                                                .col = mc});
        }
    }
    index.class_by_name.try_emplace(entry.name, index.classes.size());
    index.classes.push_back(std::move(entry));
}

void process_typedef(const TypedefDeclarationSyntax& td, SyntaxIndex& index,
                     const slang::SourceManager& sm, std::string parent_scope = {}) {
    TypedefEntry entry;
    entry.name = tok_text(td.name);
    entry.parent_scope = std::move(parent_scope);
    entry.file_id = token_file_id_for_dynamic_index(index, sm, td.name);
    auto [line, col] = token_pos(sm, td.name);
    entry.line = line;
    entry.col = col;
    if (const auto* enum_type = td.type->as_if<EnumTypeSyntax>()) {
        entry.is_enum = true;
        for (const auto* member : enum_type->members) {
            if (member) {
                auto [em_line, em_col] = token_pos(sm, member->name);
                entry.enum_members.push_back(EnumMemberEntry{
                    .name = tok_text(member->name),
                    .file_id = token_file_id_for_dynamic_index(index, sm, member->name),
                    .line = em_line,
                    .col = em_col,
                });
            }
        }
    } else if (const auto* struct_type = td.type->as_if<StructUnionTypeSyntax>()) {
        entry.is_struct = true;
        for (const auto* member : struct_type->members) {
            if (!member)
                continue;
            const auto type = node_text(*member->type);
            for (const auto* decl : member->declarators) {
                if (!decl)
                    continue;
                auto [fl, fc] = token_pos(sm, decl->name);
                entry.fields.push_back(FieldEntry{.name = tok_text(decl->name),
                                                  .type = with_dims(sm, type, *decl),
                                                  .file_id = token_file_id_for_dynamic_index(index, sm, decl->name),
                                                  .line = fl,
                                                  .col = fc});
            }
        }
    } else {
        entry.resolved = node_text(*td.type);
    }

    index.typedef_by_name.try_emplace(entry.name, index.typedefs.size());
    index.typedefs.push_back(std::move(entry));
}

void process_module(const ModuleDeclarationSyntax& node, SyntaxIndex& index,
                    const slang::SourceManager& sm, std::string_view source) {
    ModuleEntry module;
    module.name = tok_text(node.header->name);
    module.file_id = token_file_id_for_dynamic_index(index, sm, node.header->name);
    auto [line, col] = token_pos(sm, node.header->name);
    module.line = line;
    module.col = col;
    fill_module_edit_ranges(module, *node.header, sm);

    if (node.header->parameters) {
        for (const auto* base : node.header->parameters->declarations) {
            const auto* param = base ? base->as_if<ParameterDeclarationSyntax>() : nullptr;
            if (!param)
                continue;
            const std::string direction = tok_text(param->keyword);
            const std::string type = node_text(*param->type);
            for (const auto* decl : param->declarators) {
                if (!decl)
                    continue;
                std::string default_value;
                if (decl->initializer)
                    default_value = node_text(*decl->initializer->expr);
                add_port(module, index, sm, decl->name, direction, type, type, {},
                         std::move(default_value));
            }
        }
    }

    if (node.header->ports) {
        if (const auto* ansi = node.header->ports->as_if<AnsiPortListSyntax>()) {
            for (const auto* item : ansi->ports) {
                if (const auto* implicit = item ? item->as_if<ImplicitAnsiPortSyntax>() : nullptr) {
                    const auto type =
                        with_dims(sm, port_type(sm, *implicit->header), *implicit->declarator);
                    const auto decl_type =
                        with_dims(sm, port_decl_type(sm, *implicit->header), *implicit->declarator);
                    const auto signal_type =
                        with_dims(sm, port_signal_decl_type(sm, *implicit->header),
                                  *implicit->declarator);
                    add_port(module, index, sm, implicit->declarator->name,
                             port_direction(*implicit->header), type, decl_type, signal_type);
                } else if (const auto* explicit_port =
                               item ? item->as_if<ExplicitAnsiPortSyntax>() : nullptr) {
                    add_port(module, index, sm, explicit_port->name, tok_text(explicit_port->direction),
                             {});
                }
            }
        }
    }

    const auto lines = split_lines(source);
    for (const auto* member : node.members) {
        if (!member)
            continue;
        if (const auto* port_decl = member->as_if<PortDeclarationSyntax>()) {
            const auto direction = port_direction(*port_decl->header);
            const auto type = port_type(sm, *port_decl->header);
            for (const auto* decl : port_decl->declarators) {
                if (decl)
                    add_port(module, index, sm, decl->name, direction, with_dims(sm, type, *decl),
                             with_dims(sm, port_decl_type(sm, *port_decl->header), *decl),
                             with_dims(sm, port_signal_decl_type(sm, *port_decl->header), *decl));
            }
        } else if (const auto* data = member->as_if<DataDeclarationSyntax>()) {
            const auto type = node_text(*data->type);
            for (const auto* decl : data->declarators) {
                if (decl)
                    add_value(index, sm, decl->name, with_dims(sm, type, *decl), "variable",
                              module.name);
            }
        } else if (const auto* fn = member->as_if<FunctionDeclarationSyntax>()) {
            if (auto* value = add_value(index, sm, fn->prototype->keyword,
                                        node_text(*fn->prototype->returnType), "function",
                                        module.name))
                value->name = node_text(*fn->prototype->name);
        } else if (const auto* hierarchy = member->as_if<HierarchyInstantiationSyntax>()) {
            process_hierarchy(*hierarchy, index, sm, lines, module.name);
        } else if (const auto* modport = member->as_if<ModportDeclarationSyntax>()) {
            for (const auto* item : modport->items) {
                if (!item)
                    continue;
                auto [ml, mc] = token_pos(sm, item->name);
                module.modports.push_back(ModportEntry{.name = tok_text(item->name),
                                                       .file_id = token_file_id_for_dynamic_index(index, sm, item->name),
                                                       .line = ml,
                                                       .col = mc});
            }
        }
    }

    for (size_t i = 0; i < module.ports.size(); ++i)
        module.port_by_name.try_emplace(module.ports[i].name, i);
    index.module_by_name.try_emplace(module.name, index.modules.size());
    index.modules.push_back(std::move(module));
}

void process_module_signature(const ModuleDeclarationSyntax& node, SyntaxIndex& index,
                              const slang::SourceManager& sm) {
    ModuleEntry module;
    module.name = tok_text(node.header->name);
    module.file_id = token_file_id_for_dynamic_index(index, sm, node.header->name);
    auto [line, col] = token_pos(sm, node.header->name);
    module.line = line;
    module.col = col;
    fill_module_edit_ranges(module, *node.header, sm);

    if (node.header->parameters) {
        for (const auto* base : node.header->parameters->declarations) {
            const auto* param = base ? base->as_if<ParameterDeclarationSyntax>() : nullptr;
            if (!param)
                continue;
            const std::string direction = tok_text(param->keyword);
            const std::string type = node_text(*param->type);
            for (const auto* decl : param->declarators) {
                if (!decl)
                    continue;
                std::string default_value;
                if (decl->initializer)
                    default_value = node_text(*decl->initializer->expr);
                add_port(module, index, sm, decl->name, direction, type, type, {},
                         std::move(default_value));
            }
        }
    }

    if (node.header->ports) {
        if (const auto* ansi = node.header->ports->as_if<AnsiPortListSyntax>()) {
            for (const auto* item : ansi->ports) {
                if (const auto* implicit = item ? item->as_if<ImplicitAnsiPortSyntax>() : nullptr) {
                    const auto type =
                        with_dims(sm, port_type(sm, *implicit->header), *implicit->declarator);
                    const auto decl_type =
                        with_dims(sm, port_decl_type(sm, *implicit->header), *implicit->declarator);
                    const auto signal_type =
                        with_dims(sm, port_signal_decl_type(sm, *implicit->header),
                                  *implicit->declarator);
                    add_port(module, index, sm, implicit->declarator->name,
                             port_direction(*implicit->header), type, decl_type, signal_type);
                } else if (const auto* explicit_port =
                               item ? item->as_if<ExplicitAnsiPortSyntax>() : nullptr) {
                    add_port(module, index, sm, explicit_port->name, tok_text(explicit_port->direction),
                             {});
                }
            }
        }
    }

    for (const auto* member : node.members) {
        if (const auto* port_decl = member ? member->as_if<PortDeclarationSyntax>() : nullptr) {
            const auto direction = port_direction(*port_decl->header);
            const auto type = port_type(sm, *port_decl->header);
            for (const auto* decl : port_decl->declarators) {
                if (decl)
                    add_port(module, index, sm, decl->name, direction, with_dims(sm, type, *decl),
                             with_dims(sm, port_decl_type(sm, *port_decl->header), *decl),
                             with_dims(sm, port_signal_decl_type(sm, *port_decl->header), *decl));
            }
        }
    }

    for (size_t i = 0; i < module.ports.size(); ++i)
        module.port_by_name.try_emplace(module.ports[i].name, i);
    index.module_by_name.try_emplace(module.name, index.modules.size());
    index.modules.push_back(std::move(module));
}

void process_package(const ModuleDeclarationSyntax& pkg, SyntaxIndex& index,
                     const slang::SourceManager& sm) {
    ModuleEntry module;
    module.name = tok_text(pkg.header->name);
    module.file_id = token_file_id_for_dynamic_index(index, sm, pkg.header->name);
    auto [line, col] = token_pos(sm, pkg.header->name);
    module.line = line;
    module.col = col;
    index.package_names.insert(module.name);

    for (const auto* member : pkg.members) {
        if (!member)
            continue;
        if (const auto* data = member->as_if<DataDeclarationSyntax>()) {
            const auto type = node_text(*data->type);
            for (const auto* decl : data->declarators) {
                if (!decl)
                    continue;
                add_value(index, sm, decl->name, with_dims(sm, type, *decl), "variable", module.name);
                index.package_symbols[module.name].push_back(tok_text(decl->name));
            }
        } else if (const auto* ps = member->as_if<ParameterDeclarationStatementSyntax>()) {
            if (const auto* param = ps->parameter->as_if<ParameterDeclarationSyntax>()) {
                const auto type = node_text(*param->type);
                for (const auto* decl : param->declarators) {
                    if (!decl)
                        continue;
                    auto [pl, pc] = token_pos(sm, decl->name);
                    index.values.push_back(ValueEntry{.name = tok_text(decl->name),
                                                      .type = type,
                                                      .kind = tok_text(param->keyword),
                                                      .parent_scope = module.name,
                                                      .file_id = token_file_id_for_dynamic_index(index, sm, decl->name),
                                                      .line = pl,
                                                      .col = pc});
                    index.package_symbols[module.name].push_back(tok_text(decl->name));
                }
            }
        } else if (const auto* fn = member->as_if<FunctionDeclarationSyntax>()) {
            const auto name = node_text(*fn->prototype->name);
            index.values.push_back(ValueEntry{.name = name,
                                              .type = node_text(*fn->prototype->returnType),
                                              .kind = "function",
                                              .parent_scope = module.name,
                                              .file_id = token_file_id_for_dynamic_index(index, sm, fn->prototype->keyword)});
            index.package_symbols[module.name].push_back(name);
        } else if (const auto* cls = member->as_if<ClassDeclarationSyntax>()) {
            process_class(*cls, index, sm, module.name);
            index.package_symbols[module.name].push_back(tok_text(cls->name));
        } else if (const auto* td = member->as_if<TypedefDeclarationSyntax>()) {
            process_typedef(*td, index, sm, module.name);
            index.package_symbols[module.name].push_back(tok_text(td->name));
        }
    }

    index.module_by_name.try_emplace(module.name, index.modules.size());
    index.modules.push_back(std::move(module));
}

void collect_imports(const SyntaxNode& root, SyntaxIndex& index, const slang::SourceManager& sm) {
    struct Visitor : SyntaxVisitor<Visitor> {
        SyntaxIndex& index;
        const slang::SourceManager& sm;
        explicit Visitor(SyntaxIndex& index, const slang::SourceManager& sm) : index(index), sm(sm) {}
        void handle(const PackageImportDeclarationSyntax& node) {
            auto [line, _] = token_pos(sm, node.keyword);
            for (const auto* item : node.items) {
                if (!item)
                    continue;
                ImportEntry entry;
                entry.package_name = tok_text(item->package);
                entry.wildcard = item->item.kind == slang::parsing::TokenKind::Star;
                if (!entry.wildcard)
                    entry.symbol_name = tok_text(item->item);
                entry.file_id = token_file_id_for_dynamic_index(index, sm, item->package);
                entry.start_line = line;
                index.imports.push_back(std::move(entry));
            }
            visitDefault(node);
        }
    } visitor(index, sm);
    root.visit(visitor);
}

void collect_macros(const slang::syntax::SyntaxTree& tree, SyntaxIndex& index) {
    const auto& sm = tree.sourceManager();
    for (const auto* def : tree.getDefinedMacros()) {
        if (!def || !def->name || !def->name.location().valid() || !sm.isFileLoc(def->name.location()))
            continue;
        auto [line, _] = token_pos(sm, def->name);
        MacroEntry mac;
        mac.name = tok_text(def->name);
        mac.file_id = token_file_id_for_dynamic_index(index, sm, def->name);
        mac.line = line;
        if (def->formalArguments) {
            mac.is_function_like = true;
            for (const auto* arg : def->formalArguments->args) {
                if (arg)
                    mac.params.push_back(tok_text(arg->name));
            }
        }
        index.macros.push_back(std::move(mac));
    }
}

std::optional<std::string> source_text_for_dynamic_range(const slang::SourceManager& sm,
                                                         slang::SourceRange range) {
    if (!range.start().valid() || !range.end().valid())
        return std::nullopt;
    if (range.start().buffer() != range.end().buffer())
        return std::nullopt;

    const auto source = sm.getSourceText(range.start().buffer());
    const size_t begin = range.start().offset();
    const size_t end = range.end().offset();
    if (begin > end || end > source.size())
        return std::nullopt;
    return std::string(source.substr(begin, end - begin));
}

void collect_macro_reference_occurrences(const slang::syntax::SyntaxTree& tree, SyntaxIndex& index) {
    const auto& sm = tree.sourceManager();

    auto add_macro_ref = [&](std::string name, SourceFileID file_id, int line, int col) {
        if (name.empty())
            return;
        index.references.push_back(ReferenceEntry{
            .name = name,
            .file_id = file_id,
            .symbol_id = SymbolID::from_canonical("macro::" + name),
            .symbol_debug = "macro::" + name,
            .line = line,
            .col = col,
            .end_col = col + (int)name.size(),
        });
    };

    // `define names are preprocessor facts, not normal syntax-tree Identifier
    // tokens, so add declaration occurrences explicitly.
    for (const auto* def : tree.getDefinedMacros()) {
        if (!def || !def->name || !def->name.location().valid() || !sm.isFileLoc(def->name.location()))
            continue;
        const auto [line, col] = token_pos(sm, def->name);
        add_macro_ref(tok_text(def->name), token_file_id_for_dynamic_index(index, sm, def->name),
                      line, col);
    }

    struct Visitor : public SyntaxVisitor<Visitor> {
        SyntaxIndex& index;
        const slang::SourceManager& sm;
        decltype(add_macro_ref)& add_ref;
        std::unordered_set<std::string> seen_expansions;

        Visitor(SyntaxIndex& index, const slang::SourceManager& sm,
                decltype(add_macro_ref)& add_macro_ref)
            : index(index), sm(sm), add_ref(add_macro_ref) {}

        void visitToken(slang::parsing::Token token) {
            if (!token || !token.location().valid() || !sm.isMacroLoc(token.location()))
                return;

            const auto macro_name = sm.getMacroName(token.location());
            if (macro_name.empty())
                return;

            const auto range = sm.getExpansionRange(token.location());
            if (!range.start().valid())
                return;

            const auto uri = location_uri_for_dynamic_index(sm, range.start());
            const std::string key = uri + ":" + std::to_string(range.start().offset()) + ":" +
                                    std::to_string(range.end().offset());
            if (!seen_expansions.insert(key).second)
                return;

            const int line = (int)sm.getLineNumber(range.start());
            int col = (int)sm.getColumnNumber(range.start()) - 1;
            if (col < 0)
                col = 0;
            if (auto text = source_text_for_dynamic_range(sm, range); text && text->starts_with('`'))
                ++col;

            add_ref(std::string(macro_name),
                          location_file_id_for_dynamic_index(index, sm, range.start()), line, col);
        }
    };

    Visitor visitor(index, sm, add_macro_ref);
    tree.root().visit(visitor);
}

void collect_reference_occurrences(const SyntaxNode& root, SyntaxIndex& index,
                                   const slang::SourceManager& sm) {
    std::unordered_set<std::string> module_values;
    std::unordered_map<std::string, std::string> module_value_types;
    std::unordered_set<std::string> package_values;
    std::unordered_set<std::string> class_fields;
    std::unordered_set<std::string> typedef_fields;
    std::unordered_map<std::string, std::string> unique_typedef_scopes;
    std::unordered_set<std::string> ambiguous_typedef_names;
    std::unordered_map<std::string, std::string> unique_enum_member_ids;
    std::unordered_set<std::string> ambiguous_enum_members;
    std::unordered_map<std::string, std::string> unique_type_ids;
    std::unordered_set<std::string> ambiguous_type_names;
    for (const auto& value : index.values) {
        if (value.parent_scope.empty())
            continue;
        if (index.package_names.contains(value.parent_scope)) {
            package_values.insert(value.parent_scope + "\n" + value.name);
            continue;
        }
        if (is_module_value_kind(value.kind)) {
            const auto key = value.parent_scope + "\n" + value.name;
            module_values.insert(key);
            // During editing duplicate declarations are common.  Prefer the
            // later syntactic type for member access resolution, matching the
            // full SyntaxIndex path.
            module_value_types[key] = canonical_type_name_from_text(value.type);
        }
    }
    for (const auto& cls : index.classes) {
        const std::string class_scope =
            cls.parent_scope.empty() ? cls.name : cls.parent_scope + "::" + cls.name;
        const auto class_id = symbol_canonical("class", cls.parent_scope, cls.name);
        if (!ambiguous_type_names.contains(cls.name) &&
            !unique_type_ids.try_emplace(cls.name, class_id).second) {
            unique_type_ids.erase(cls.name);
            ambiguous_type_names.insert(cls.name);
        }
        for (const auto& field : cls.fields)
            class_fields.insert(class_scope + "\n" + field.name);
    }
    for (const auto& td : index.typedefs) {
        const auto typedef_id = symbol_canonical("typedef", td.parent_scope, td.name);
        if (!ambiguous_type_names.contains(td.name) &&
            !unique_type_ids.try_emplace(td.name, typedef_id).second) {
            unique_type_ids.erase(td.name);
            ambiguous_type_names.insert(td.name);
        }
        const auto typedef_scope =
            td.parent_scope.empty() ? td.name : td.parent_scope + "::" + td.name;
        if (!ambiguous_typedef_names.contains(td.name) &&
            !unique_typedef_scopes.try_emplace(td.name, typedef_scope).second) {
            unique_typedef_scopes.erase(td.name);
            ambiguous_typedef_names.insert(td.name);
        }
        for (const auto& field : td.fields)
            typedef_fields.insert(typedef_scope + "\n" + field.name);
        if (td.is_enum) {
            for (const auto& member : td.enum_members) {
                const auto member_id = symbol_canonical("enum_member", typedef_scope, member.name);
                if (!ambiguous_enum_members.contains(member.name) &&
                    !unique_enum_member_ids.try_emplace(member.name, member_id).second) {
                    unique_enum_member_ids.erase(member.name);
                    ambiguous_enum_members.insert(member.name);
                }
            }
        }
    }

    auto add_reference = [&](const slang::parsing::Token& token, std::string canonical_id) {
        if (!token || token.kind != slang::parsing::TokenKind::Identifier ||
            !token.location().valid())
            return;

        auto [line, col] = token_pos(sm, token);
        const std::string name(token.valueText());
        if (name.empty())
            return;

        index.references.push_back(ReferenceEntry{
            .name = name,
            .file_id = token_file_id_for_dynamic_index(index, sm, token),
            .symbol_id = SymbolID::from_canonical(canonical_id),
            .symbol_debug = std::move(canonical_id),
            .line = line,
            .col = col,
            .end_col = col + (int)name.size(),
        });
    };

    for (const auto& module : index.modules) {
        if (!module.name.empty() && module.line > 0)
            index.references.push_back(ReferenceEntry{.name = module.name,
                                                      .file_id = module.file_id,
                                                      .symbol_id = SymbolID::from_canonical(
                                                          symbol_canonical("module", {}, module.name)),
                                                      .symbol_debug = symbol_canonical("module", {}, module.name),
                                                      .line = module.line,
                                                      .col = module.col,
                                                      .end_col = module.col +
                                                                 (int)module.name.size()});
        for (const auto& port : module.ports) {
            if (!port.name.empty() && port.line > 0) {
                const bool is_parameter =
                    port.direction == "parameter" || port.direction == "localparam";
                index.references.push_back(ReferenceEntry{
                    .name = port.name,
                    .file_id = port.file_id,
                    .symbol_id = SymbolID::from_canonical(symbol_canonical(
                        is_parameter ? "module_param" : "module_port", module.name, port.name)),
                    .symbol_debug = symbol_canonical(
                        is_parameter ? "module_param" : "module_port", module.name, port.name),
                    .line = port.line,
                    .col = port.col,
                    .end_col = port.col + (int)port.name.size()});
            }
        }
    }

    for (const auto& value : index.values) {
        if (value.name.empty() || value.parent_scope.empty() || !is_module_value_kind(value.kind) ||
            value.line <= 0 || index.package_names.contains(value.parent_scope))
            continue;
        const auto canonical = symbol_canonical("module_signal", value.parent_scope, value.name);
        index.references.push_back(ReferenceEntry{.name = value.name,
                                                  .file_id = value.file_id,
                                                  .symbol_id = SymbolID::from_canonical(canonical),
                                                  .symbol_debug = canonical,
                                                  .line = value.line,
                                                  .col = value.col,
                                                  .end_col = value.col +
                                                             (int)value.name.size()});
    }
    for (const auto& value : index.values) {
        if (value.name.empty() || value.parent_scope.empty() ||
            !index.package_names.contains(value.parent_scope) || value.line <= 0)
            continue;
        const auto canonical = symbol_canonical("package_value", value.parent_scope, value.name);
        index.references.push_back(ReferenceEntry{.name = value.name,
                                                  .file_id = value.file_id,
                                                  .symbol_id = SymbolID::from_canonical(canonical),
                                                  .symbol_debug = canonical,
                                                  .line = value.line,
                                                  .col = value.col,
                                                  .end_col = value.col +
                                                             (int)value.name.size()});
    }
    for (const auto& cls : index.classes) {
        if (cls.name.empty() || cls.line <= 0)
            continue;
        const auto class_scope =
            cls.parent_scope.empty() ? cls.name : cls.parent_scope + "::" + cls.name;
        const auto class_canonical = symbol_canonical("class", cls.parent_scope, cls.name);
        index.references.push_back(ReferenceEntry{.name = cls.name,
                                                  .file_id = cls.file_id,
                                                  .symbol_id =
                                                      SymbolID::from_canonical(class_canonical),
                                                  .symbol_debug = class_canonical,
                                                  .line = cls.line,
                                                  .col = cls.col,
                                                  .end_col = cls.col + (int)cls.name.size()});
        for (const auto& field : cls.fields) {
            if (field.name.empty() || field.line <= 0)
                continue;
            const auto canonical = symbol_canonical("class_field", class_scope, field.name);
            index.references.push_back(ReferenceEntry{.name = field.name,
                                                      .file_id = field.file_id,
                                                      .symbol_id =
                                                          SymbolID::from_canonical(canonical),
                                                      .symbol_debug = canonical,
                                                      .line = field.line,
                                                      .col = field.col,
                                                      .end_col = field.col +
                                                                 (int)field.name.size()});
        }
    }
    for (const auto& td : index.typedefs) {
        if (td.name.empty() || td.line <= 0)
            continue;
        const auto typedef_scope =
            td.parent_scope.empty() ? td.name : td.parent_scope + "::" + td.name;
        const auto canonical = symbol_canonical("typedef", td.parent_scope, td.name);
        index.references.push_back(ReferenceEntry{.name = td.name,
                                                  .file_id = td.file_id,
                                                  .symbol_id = SymbolID::from_canonical(canonical),
                                                  .symbol_debug = canonical,
                                                  .line = td.line,
                                                  .col = td.col,
                                                  .end_col = td.col + (int)td.name.size()});
        for (const auto& field : td.fields) {
            if (field.name.empty() || field.line <= 0)
                continue;
            const auto field_canonical =
                symbol_canonical("typedef_field", typedef_scope, field.name);
            index.references.push_back(ReferenceEntry{.name = field.name,
                                                      .file_id = field.file_id,
                                                      .symbol_id =
                                                          SymbolID::from_canonical(field_canonical),
                                                      .symbol_debug = field_canonical,
                                                      .line = field.line,
                                                      .col = field.col,
                                                      .end_col = field.col +
                                                                 (int)field.name.size()});
        }
        for (const auto& member : td.enum_members) {
            if (member.name.empty() || member.line <= 0)
                continue;
            const auto member_canonical =
                symbol_canonical("enum_member", typedef_scope, member.name);
            index.references.push_back(ReferenceEntry{.name = member.name,
                                                      .file_id = member.file_id,
                                                      .symbol_id =
                                                          SymbolID::from_canonical(member_canonical),
                                                      .symbol_debug = member_canonical,
                                                      .line = member.line,
                                                      .col = member.col,
                                                      .end_col = member.col +
                                                                 (int)member.name.size()});
        }
    }

    struct Visitor : SyntaxVisitor<Visitor> {
        SyntaxIndex& index;
        const slang::SourceManager& sm;
        decltype(add_reference)& add_ref;
        const std::unordered_set<std::string>& module_values;
        const std::unordered_map<std::string, std::string>& module_value_types;
        const std::unordered_set<std::string>& package_values;
        const std::unordered_set<std::string>& class_fields;
        const std::unordered_set<std::string>& typedef_fields;
        const std::unordered_map<std::string, std::string>& unique_typedef_scopes;
        const std::unordered_map<std::string, std::string>& unique_enum_member_ids;
        const std::unordered_map<std::string, std::string>& unique_type_ids;
        std::string current_module;
        std::string current_package;
        std::string current_class;

        explicit Visitor(SyntaxIndex& index, const slang::SourceManager& sm,
                         decltype(add_reference)& add_reference,
                         const std::unordered_set<std::string>& module_values,
                         const std::unordered_map<std::string, std::string>& module_value_types,
                         const std::unordered_set<std::string>& package_values,
                         const std::unordered_set<std::string>& class_fields,
                         const std::unordered_set<std::string>& typedef_fields,
                         const std::unordered_map<std::string, std::string>& unique_typedef_scopes,
                         const std::unordered_map<std::string, std::string>& unique_enum_member_ids,
                         const std::unordered_map<std::string, std::string>& unique_type_ids)
            : index(index), sm(sm), add_ref(add_reference), module_values(module_values),
              module_value_types(module_value_types), package_values(package_values),
              class_fields(class_fields), typedef_fields(typedef_fields),
              unique_typedef_scopes(unique_typedef_scopes),
              unique_enum_member_ids(unique_enum_member_ids),
              unique_type_ids(unique_type_ids) {}

        void handle(const ModuleDeclarationSyntax& node) {
            const std::string module_name(node.header->name.valueText());
            if (!module_name.empty())
                add_ref(node.header->name, symbol_canonical("module", {}, module_name));

            auto previous_module = current_module;
            auto previous_package = current_package;
            if (node.kind == SyntaxKind::PackageDeclaration) {
                current_package = module_name;
                current_module.clear();
            } else {
                current_module = module_name;
            }
            visitDefault(node);
            current_module = std::move(previous_module);
            current_package = std::move(previous_package);
        }

        void handle(const ClassDeclarationSyntax& node) {
            const std::string class_name(node.name.valueText());
            const std::string parent_scope =
                !current_package.empty() ? current_package : std::string{};
            if (!class_name.empty())
                add_ref(node.name, symbol_canonical("class", parent_scope, class_name));

            auto previous_class = current_class;
            current_class = parent_scope.empty() ? class_name : parent_scope + "::" + class_name;
            visitDefault(node);
            current_class = std::move(previous_class);
        }

        void handle(const TypedefDeclarationSyntax& node) {
            const std::string typedef_name(node.name.valueText());
            const std::string parent_scope =
                !current_package.empty() ? current_package : std::string{};
            if (!typedef_name.empty())
                add_ref(node.name, symbol_canonical("typedef", parent_scope, typedef_name));
            visitDefault(node);
        }

        void handle(const HierarchyInstantiationSyntax& node) {
            const std::string module_name(node.type.valueText());
            if (!module_name.empty())
                add_ref(node.type, symbol_canonical("module", {}, module_name));

            if (node.parameters) {
                for (const auto* parameter : node.parameters->parameters) {
                    const auto* named =
                        parameter ? parameter->as_if<NamedParamAssignmentSyntax>() : nullptr;
                    if (!named)
                        continue;
                    const std::string param_name(named->name.valueText());
                    if (!module_name.empty() && !param_name.empty())
                        add_ref(named->name,
                                symbol_canonical("module_param", module_name, param_name));
                }
            }

            for (const auto* inst : node.instances) {
                if (!inst)
                    continue;

                if (inst->decl) {
                    const std::string instance_name(inst->decl->name.valueText());
                    if (!current_module.empty() && !instance_name.empty())
                        add_ref(inst->decl->name,
                                symbol_canonical("instance", current_module, instance_name));
                }

                for (const auto* conn : inst->connections) {
                    const auto* named = conn ? conn->as_if<NamedPortConnectionSyntax>() : nullptr;
                    if (!named)
                        continue;
                    const std::string port_name(named->name.valueText());
                    if (!module_name.empty() && !port_name.empty())
                        add_ref(named->name, symbol_canonical("module_port", module_name, port_name));
                }
            }

            visitDefault(node);
        }

        void handle(const MemberAccessExpressionSyntax& node) {
            const std::string field_name(node.name.valueText());
            const std::string object_name = simple_identifier_from_expr_node(node.left);
            if (!current_module.empty() && !field_name.empty() && !object_name.empty()) {
                const auto value_it = module_value_types.find(current_module + "\n" + object_name);
                if (value_it != module_value_types.end()) {
                    std::string typedef_scope = value_it->second;
                    if (const auto scope_it = unique_typedef_scopes.find(typedef_scope);
                        scope_it != unique_typedef_scopes.end())
                        typedef_scope = scope_it->second;

                    if (typedef_fields.contains(typedef_scope + "\n" + field_name)) {
                        add_ref(node.name,
                                symbol_canonical("typedef_field", typedef_scope, field_name));
                        if (node.left)
                            node.left->visit(*this);
                        return;
                    }
                }
            }
            visitDefault(node);
        }

        static bool wordlike(char c) {
            return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$' || c == '`';
        }

        std::string object_before_member_dot(const slang::parsing::Token& token) const {
            if (!token || !token.location().valid())
                return {};
            const auto source = sm.getSourceText(token.location().buffer());
            size_t i = token.location().offset();
            if (i > source.size())
                return {};
            while (i > 0 && std::isspace(static_cast<unsigned char>(source[i - 1])))
                --i;
            if (i == 0 || source[i - 1] != '.')
                return {};
            --i;
            while (i > 0 && std::isspace(static_cast<unsigned char>(source[i - 1])))
                --i;
            const size_t end = i;
            while (i > 0 && wordlike(source[i - 1]))
                --i;
            if (i == end)
                return {};
            return std::string(source.substr(i, end - i));
        }

        bool try_add_typedef_field_reference(const slang::parsing::Token& token,
                                             std::string_view field_name) {
            const auto object_name = object_before_member_dot(token);
            if (current_module.empty() || object_name.empty() || field_name.empty())
                return false;
            const auto value_it = module_value_types.find(current_module + "\n" + object_name);
            if (value_it == module_value_types.end())
                return false;

            std::string typedef_scope = value_it->second;
            if (const auto scope_it = unique_typedef_scopes.find(typedef_scope);
                scope_it != unique_typedef_scopes.end())
                typedef_scope = scope_it->second;

            if (!typedef_fields.contains(typedef_scope + "\n" + std::string(field_name)))
                return false;

            add_ref(token, symbol_canonical("typedef_field", typedef_scope, std::string(field_name)));
            return true;
        }

        void visitToken(slang::parsing::Token token) {
            if (!token || token.kind != slang::parsing::TokenKind::Identifier ||
                !token.location().valid())
                return;
            const std::string name(token.valueText());
            if (name.empty())
                return;
            if (try_add_typedef_field_reference(token, name))
                return;
            if (!current_class.empty()) {
                const std::string key = current_class + "\n" + name;
                if (class_fields.contains(key)) {
                    add_ref(token, symbol_canonical("class_field", current_class, name));
                    return;
                }
            }
            if (!current_module.empty()) {
                const std::string key = current_module + "\n" + name;
                if (module_values.contains(key)) {
                    add_ref(token, symbol_canonical("module_signal", current_module, name));
                    return;
                }
            }
            if (!current_package.empty()) {
                const std::string key = current_package + "\n" + name;
                if (package_values.contains(key)) {
                    add_ref(token, symbol_canonical("package_value", current_package, name));
                    return;
                }
            }
            if (const auto type_it = unique_type_ids.find(name); type_it != unique_type_ids.end()) {
                add_ref(token, type_it->second);
                return;
            }
            if (const auto enum_it = unique_enum_member_ids.find(name);
                enum_it != unique_enum_member_ids.end()) {
                add_ref(token, enum_it->second);
                return;
            }
            add_ref(token, "name:" + name);
        }
    };

    Visitor visitor(index, sm, add_reference, module_values, module_value_types, package_values,
                    class_fields, typedef_fields, unique_typedef_scopes, unique_enum_member_ids,
                    unique_type_ids);
    root.visit(visitor);
}

} // namespace

SyntaxIndex build_current_ast_structural_index(const DocumentState& state) {
    SyntaxIndex index;
    if (!state.tree)
        return index;

    const auto& root = state.tree->root();
    const auto& sm = state.tree->sourceManager();
    auto process_member = [&](const MemberSyntax& member) {
        if (const auto* mod = member.as_if<ModuleDeclarationSyntax>()) {
            if (member.kind == SyntaxKind::InterfaceDeclaration) {
                process_module(*mod, index, sm, state.text);
                index.interface_names.insert(tok_text(mod->header->name));
            } else if (member.kind == SyntaxKind::PackageDeclaration) {
                process_package(*mod, index, sm);
            } else {
                process_module(*mod, index, sm, state.text);
            }
        } else if (const auto* cls = member.as_if<ClassDeclarationSyntax>())
            process_class(*cls, index, sm);
        else if (const auto* td = member.as_if<TypedefDeclarationSyntax>())
            process_typedef(*td, index, sm);
    };

    if (const auto* cu = root.as_if<CompilationUnitSyntax>()) {
        for (const auto* member : cu->members) {
            if (member)
                process_member(*member);
        }
    } else if (const auto* member = root.as_if<MemberSyntax>()) {
        process_member(*member);
    }

    collect_reference_occurrences(root, index, sm);
    collect_macro_reference_occurrences(*state.tree, index);
    return index;
}

const SyntaxIndex& get_structural_index(const DocumentState& state) {
    std::call_once(state.structural_index_once_, [&state] {
        state.structural_index_cache_ = build_current_ast_structural_index(state);
    });
    return state.structural_index_cache_;
}

SyntaxIndex build_dynamic_file_index(const DocumentState& state) {
    SyntaxIndex index = get_structural_index(state);
    if (!state.tree)
        return index;

    collect_imports(state.tree->root(), index, state.tree->sourceManager());
    collect_macros(*state.tree, index);
    // The structural cache already contains macro reference occurrences for
    // the open file.  `collect_macros()` above only adds completion metadata
    // (MacroEntry), so do not add references a second time here.
    return index;
}

const SyntaxIndex& get_dynamic_index(const DocumentState& state) {
    std::call_once(state.dynamic_index_once_, [&state] {
        state.dynamic_index_cache_ = build_dynamic_file_index(state);
    });
    return state.dynamic_index_cache_;
}
