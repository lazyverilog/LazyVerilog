#include "dynamic_file_index.hpp"
#include "syntax_index_shared.hpp"
#include "string_utils.hpp"

#include <algorithm>
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

std::string node_text(const SyntaxNode& node) {
    return trim_copy(node.toString());
}

std::string node_text_raw(const slang::SourceManager& sm, const SyntaxNode& node) {
    const auto range = node.sourceRange();
    if (range.start().valid() && range.end().valid() &&
        range.start().buffer() == range.end().buffer()) {
        const auto source = sm.getSourceText(range.start().buffer());
        const size_t begin = range.start().offset();
        const size_t end = range.end().offset();
        if (begin <= end && end <= source.size())
            return trim_copy(std::string(source.substr(begin, end - begin)));
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
                                      .file_id = source_file_id_for_token(index, sm, name),
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
                                     .file_id = source_file_id_for_token(index, sm, name),
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
                                      .file_id = source_file_id_for_token(index, sm, name),
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
            entry.file_id = source_file_id_for_token(index, sm, inst->decl->name);
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
                                                          .file_id = source_file_id_for_token(index, sm, named->name),
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
    entry.file_id = source_file_id_for_token(index, sm, cls.name);
    entry.parent_scope = std::move(parent_scope);
    auto [line, col] = token_pos(sm, cls.name);
    entry.line = line;
    entry.col = col;
    if (cls.extendsClause)
        entry.base_class = node_text_raw(sm, *cls.extendsClause->baseName);

    for (const auto* item : cls.items) {
        if (!item)
            continue;
        if (const auto* prop = item->as_if<ClassPropertyDeclarationSyntax>()) {
            if (const auto* data = prop->declaration->as_if<DataDeclarationSyntax>()) {
                const auto type = node_text_raw(sm, *data->type);
                for (const auto* decl : data->declarators) {
                    if (!decl)
                        continue;
                    auto [fl, fc] = token_pos(sm, decl->name);
                    entry.fields.push_back(FieldEntry{.name = tok_text(decl->name),
                                                      .type = with_dims(sm, type, *decl),
                                                      .file_id = source_file_id_for_token(index, sm, decl->name),
                                                      .line = fl,
                                                      .col = fc});
                }
            }
        } else if (const auto* method = item->as_if<ClassMethodDeclarationSyntax>()) {
            const auto& proto = *method->declaration->prototype;
            auto [ml, mc] = token_pos(sm, proto.keyword);
            entry.methods.push_back(MethodEntry{.name = node_text_raw(sm, *proto.name),
                                                .return_type = node_text_raw(sm, *proto.returnType),
                                                .is_task = method->declaration->kind ==
                                                           SyntaxKind::TaskDeclaration,
                                                .file_id = source_file_id_for_token(index, sm, proto.keyword),
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
    entry.file_id = source_file_id_for_token(index, sm, td.name);
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
                    .file_id = source_file_id_for_token(index, sm, member->name),
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
            const auto type = node_text_raw(sm, *member->type);
            for (const auto* decl : member->declarators) {
                if (!decl)
                    continue;
                auto [fl, fc] = token_pos(sm, decl->name);
                entry.fields.push_back(FieldEntry{.name = tok_text(decl->name),
                                                  .type = with_dims(sm, type, *decl),
                                                  .file_id = source_file_id_for_token(index, sm, decl->name),
                                                  .line = fl,
                                                  .col = fc});
            }
        }
    } else {
        entry.resolved = node_text_raw(sm, *td.type);
    }

    index.typedef_by_name.try_emplace(entry.name, index.typedefs.size());
    index.typedefs.push_back(std::move(entry));
}

void process_module(const ModuleDeclarationSyntax& node, SyntaxIndex& index,
                    const slang::SourceManager& sm, std::string_view source) {
    ModuleEntry module;
    module.name = tok_text(node.header->name);
    module.file_id = source_file_id_for_token(index, sm, node.header->name);
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
            const std::string type = node_text_raw(sm, *param->type);
            for (const auto* decl : param->declarators) {
                if (!decl)
                    continue;
                std::string default_value;
                if (decl->initializer)
                    default_value = node_text_raw(sm, *decl->initializer->expr);
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

    const auto lines = split_lines_view(source);
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
            const auto type = node_text_raw(sm, *data->type);
            for (const auto* decl : data->declarators) {
                if (decl)
                    add_value(index, sm, decl->name, with_dims(sm, type, *decl), "variable",
                              module.name);
            }
        } else if (const auto* fn = member->as_if<FunctionDeclarationSyntax>()) {
            if (auto* value = add_value(index, sm, fn->prototype->keyword,
                                        node_text_raw(sm, *fn->prototype->returnType), "function",
                                        module.name))
                value->name = node_text_raw(sm, *fn->prototype->name);
        } else if (const auto* hierarchy = member->as_if<HierarchyInstantiationSyntax>()) {
            process_hierarchy(*hierarchy, index, sm, lines, module.name);
        } else if (const auto* modport = member->as_if<ModportDeclarationSyntax>()) {
            for (const auto* item : modport->items) {
                if (!item)
                    continue;
                auto [ml, mc] = token_pos(sm, item->name);
                module.modports.push_back(ModportEntry{.name = tok_text(item->name),
                                                       .file_id = source_file_id_for_token(index, sm, item->name),
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
    module.file_id = source_file_id_for_token(index, sm, node.header->name);
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
            const std::string type = node_text_raw(sm, *param->type);
            for (const auto* decl : param->declarators) {
                if (!decl)
                    continue;
                std::string default_value;
                if (decl->initializer)
                    default_value = node_text_raw(sm, *decl->initializer->expr);
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
    module.file_id = source_file_id_for_token(index, sm, pkg.header->name);
    auto [line, col] = token_pos(sm, pkg.header->name);
    module.line = line;
    module.col = col;
    index.package_names.insert(module.name);

    for (const auto* member : pkg.members) {
        if (!member)
            continue;
        if (const auto* data = member->as_if<DataDeclarationSyntax>()) {
            const auto type = node_text_raw(sm, *data->type);
            for (const auto* decl : data->declarators) {
                if (!decl)
                    continue;
                add_value(index, sm, decl->name, with_dims(sm, type, *decl), "variable", module.name);
                index.package_symbols[module.name].push_back(tok_text(decl->name));
            }
        } else if (const auto* ps = member->as_if<ParameterDeclarationStatementSyntax>()) {
            if (const auto* param = ps->parameter->as_if<ParameterDeclarationSyntax>()) {
                const auto type = node_text_raw(sm, *param->type);
                for (const auto* decl : param->declarators) {
                    if (!decl)
                        continue;
                    auto [pl, pc] = token_pos(sm, decl->name);
                    index.values.push_back(ValueEntry{.name = tok_text(decl->name),
                                                      .type = type,
                                                      .kind = tok_text(param->keyword),
                                                      .parent_scope = module.name,
                                                      .file_id = source_file_id_for_token(index, sm, decl->name),
                                                      .line = pl,
                                                      .col = pc});
                    index.package_symbols[module.name].push_back(tok_text(decl->name));
                }
            }
        } else if (const auto* fn = member->as_if<FunctionDeclarationSyntax>()) {
            const auto name = node_text_raw(sm, *fn->prototype->name);
            index.values.push_back(ValueEntry{.name = name,
                                              .type = node_text_raw(sm, *fn->prototype->returnType),
                                              .kind = "function",
                                              .parent_scope = module.name,
                                              .file_id = source_file_id_for_token(index, sm, fn->prototype->keyword)});
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
                entry.file_id = source_file_id_for_token(index, sm, item->package);
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
        mac.file_id = source_file_id_for_token(index, sm, def->name);
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
        add_macro_ref(tok_text(def->name), source_file_id_for_token(index, sm, def->name),
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

            const auto uri = uri_from_source_location(sm, range.start());
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
                          source_file_id_for_location(index, sm, range.start()), line, col);
        }
    };

    Visitor visitor(index, sm, add_macro_ref);
    tree.root().visit(visitor);
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
    index.include_dependencies = collect_include_dependency_uris(sm, state.uri);
    index.rebuild_reference_location_lookup();
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
