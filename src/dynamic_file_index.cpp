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

std::pair<int, int> token_pos0(const slang::SourceManager& sm,
                               const slang::parsing::Token& token) {
    auto [line, col] = token_pos_line1_col0(sm, token);
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

ValueEntry* add_value(SyntaxIndex& index, SourceFileIdResolver& resolver,
                      const slang::SourceManager& sm,
                      const slang::parsing::Token& name, std::string type, std::string kind,
                      std::string parent_scope) {
    if (!name)
        return nullptr;
    auto [line, col] = token_pos_line1_col0(sm, name);
    index.values.push_back(ValueEntry{.name = token_value_text(name),
                                      .type = std::move(type),
                                      .kind = std::move(kind),
                                      .parent_scope = std::move(parent_scope),
                                      .file_id = resolver.for_declaration_token(index, sm, name),
                                      .line = line,
                                      .col = col});
    return &index.values.back();
}

void add_port(ModuleEntry& module, SyntaxIndex& index, SourceFileIdResolver& resolver,
              const slang::SourceManager& sm,
              const slang::parsing::Token& name, std::string direction, std::string type,
              std::string decl_type = {}, std::string signal_decl_type = {},
              std::string default_value = {}) {
    if (!name)
        return;
    auto [line, col] = token_pos_line1_col0(sm, name);
    module.ports.push_back(PortEntry{.name = token_value_text(name),
                                     .file_id = resolver.for_declaration_token(index, sm, name),
                                     .direction = direction,
                                     .type = type,
                                     .decl_type = decl_type.empty() ? type : std::move(decl_type),
                                     .signal_decl_type =
                                         signal_decl_type.empty() ? type : std::move(signal_decl_type),
                                     .default_value = std::move(default_value),
                                     .line = line,
                                     .col = col});
    const bool is_parameter = direction == "parameter" || direction == "localparam";
    index.values.push_back(ValueEntry{.name = token_value_text(name),
                                      .type = std::move(type),
                                      .kind = is_parameter ? direction : std::string("port"),
                                      .parent_scope = module.name,
                                      .default_value = is_parameter
                                                           ? module.ports.back().default_value
                                                           : std::string{},
                                      .file_id = resolver.for_declaration_token(index, sm, name),
                                      .line = line,
                                      .col = col});
}

std::string port_direction(const PortHeaderSyntax& header) {
    if (const auto* variable = header.as_if<VariablePortHeaderSyntax>())
        return token_value_text(variable->direction).empty() ? "unknown" : token_value_text(variable->direction);
    if (const auto* net = header.as_if<NetPortHeaderSyntax>())
        return token_value_text(net->direction).empty() ? "unknown" : token_value_text(net->direction);
    return "unknown";
}

std::string port_type(const slang::SourceManager& sm, const PortHeaderSyntax& header) {
    if (const auto* variable = header.as_if<VariablePortHeaderSyntax>())
        return node_text_raw(sm, *variable->dataType);
    if (const auto* net = header.as_if<NetPortHeaderSyntax>())
        return node_text_raw(sm, *net->dataType);
    // `AXI_BUS.Slave bus`: mirrors type_of() in syntax_index.cpp so an open
    // buffer records the same interface port type a closed shard does.
    if (const auto* iface = header.as_if<InterfacePortHeaderSyntax>()) {
        std::string text = token_value_text(iface->nameOrKeyword);
        if (iface->modport && iface->modport->member)
            text += "." + token_value_text(iface->modport->member);
        return text;
    }
    return {};
}

std::string port_decl_type(const slang::SourceManager& sm, const PortHeaderSyntax& header) {
    if (const auto* variable = header.as_if<VariablePortHeaderSyntax>())
        return node_text_raw(sm, *variable->dataType);
    if (const auto* net = header.as_if<NetPortHeaderSyntax>()) {
        std::string text = token_value_text(net->netType);
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
                       SourceFileIdResolver& resolver, const slang::SourceManager& sm, const std::vector<std::string_view>& lines,
                       const std::string& parent_module) {
    const std::string module_name = token_value_text(hierarchy.type);
    for (const auto* inst : hierarchy.instances) {
        if (!inst)
            continue;
        InstanceEntry entry;
        entry.module_name = module_name;
        entry.parent_module = parent_module;
        if (inst->decl) {
            entry.instance_name = token_value_text(inst->decl->name);
            entry.file_id = resolver.for_declaration_token(index, sm, inst->decl->name);
            entry.line = token_pos_line1_col0(sm, inst->decl->name).first;
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
                auto [line, col] = token_pos_line1_col0(sm, named->name);
                auto [paren_line, paren_col] = token_pos_line1_col0(sm, named->openParen);
                entry.connections.push_back(NamedPortConn{.port_name = token_value_text(named->name),
                                                          .signal_name =
                                                              simple_identifier_from_expr(named->expr),
                                                          .file_id = resolver.for_declaration_token(index, sm, named->name),
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

bool is_generate_construct(const MemberSyntax& member) {
    switch (member.kind) {
        case SyntaxKind::GenerateRegion:
        case SyntaxKind::GenerateBlock:
        case SyntaxKind::LoopGenerate:
        case SyntaxKind::IfGenerate:
        case SyntaxKind::CaseGenerate:
            return true;
        default:
            return false;
    }
}

// Instantiations also live inside generate constructs, which the parser keeps
// as nested member lists rather than splicing into the enclosing module.  Walk
// those bodies for instances only; every other member kind stays scoped to the
// module-level loop in process_module().  Mirrors extract_instances() in
// syntax_index.cpp so open buffers and closed shards agree.
void process_generate_instances(const MemberSyntax& member, SyntaxIndex& index,
                                SourceFileIdResolver& resolver,
                                const slang::SourceManager& sm,
                                const std::vector<std::string_view>& lines,
                                const std::string& parent_module) {
    if (const auto* hierarchy = member.as_if<HierarchyInstantiationSyntax>()) {
        process_hierarchy(*hierarchy, index, resolver, sm, lines, parent_module);
    } else if (const auto* region = member.as_if<GenerateRegionSyntax>()) {
        for (const auto* child : region->members) {
            if (child)
                process_generate_instances(*child, index, resolver, sm, lines, parent_module);
        }
    } else if (const auto* block = member.as_if<GenerateBlockSyntax>()) {
        for (const auto* child : block->members) {
            if (child)
                process_generate_instances(*child, index, resolver, sm, lines, parent_module);
        }
    } else if (const auto* loop = member.as_if<LoopGenerateSyntax>()) {
        process_generate_instances(*loop->block, index, resolver, sm, lines, parent_module);
    } else if (const auto* cond = member.as_if<IfGenerateSyntax>()) {
        process_generate_instances(*cond->block, index, resolver, sm, lines, parent_module);
        // `else` arms and case-item bodies are typed as bare SyntaxNode because
        // they can also hold non-member syntax; only member-shaped clauses can
        // contain instantiations.
        if (cond->elseClause) {
            if (const auto* arm = cond->elseClause->clause->as_if<MemberSyntax>())
                process_generate_instances(*arm, index, resolver, sm, lines, parent_module);
        }
    } else if (const auto* sel = member.as_if<CaseGenerateSyntax>()) {
        for (const auto* item : sel->items) {
            const SyntaxNode* body = nullptr;
            if (const auto* standard = item->as_if<StandardCaseItemSyntax>())
                body = standard->clause;
            else if (const auto* def = item->as_if<DefaultCaseItemSyntax>())
                body = def->clause;
            if (const auto* arm = body ? body->as_if<MemberSyntax>() : nullptr)
                process_generate_instances(*arm, index, resolver, sm, lines, parent_module);
        }
    }
}

void process_typedef(const TypedefDeclarationSyntax& td, SyntaxIndex& index,
                     SourceFileIdResolver& resolver, const slang::SourceManager& sm,
                     std::string parent_scope, bool scoped_lookup = false);

void process_class(const ClassDeclarationSyntax& cls, SyntaxIndex& index,
                   SourceFileIdResolver& resolver, const slang::SourceManager& sm,
                   std::string parent_scope = {}) {
    ClassEntry entry;
    entry.name = token_value_text(cls.name);
    entry.file_id = resolver.for_declaration_token(index, sm, cls.name);
    entry.parent_scope = std::move(parent_scope);
    auto [line, col] = token_pos_line1_col0(sm, cls.name);
    entry.line = line;
    entry.col = col;
    if (cls.extendsClause)
        entry.base_class = node_text_raw(sm, *cls.extendsClause->baseName);

    for (const auto* item : cls.items) {
        if (!item)
            continue;
        if (const auto* prop = item->as_if<ClassPropertyDeclarationSyntax>()) {
            // `typedef registry #(T) type_id;` in a class body is a member of
            // that class, reached as `my_item::type_id`.  The shard builder
            // indexes these; the open-buffer index has to as well, or the same
            // file offers different members depending on whether it is open.
            if (const auto* nested = prop->declaration->as_if<TypedefDeclarationSyntax>()) {
                process_typedef(*nested, index, resolver, sm, entry.name,
                                /*scoped_lookup=*/true);
            } else if (const auto* data = prop->declaration->as_if<DataDeclarationSyntax>()) {
                const auto type = node_text_raw(sm, *data->type);
                for (const auto* decl : data->declarators) {
                    if (!decl)
                        continue;
                    auto [fl, fc] = token_pos_line1_col0(sm, decl->name);
                    entry.fields.push_back(FieldEntry{.name = token_value_text(decl->name),
                                                      .type = with_dims(sm, type, *decl),
                                                      .file_id = resolver.for_declaration_token(index, sm, decl->name),
                                                      .line = fl,
                                                      .col = fc});
                }
            }
        } else if (const auto* method = item->as_if<ClassMethodDeclarationSyntax>()) {
            const auto& proto = *method->declaration->prototype;
            // Match the shard builder: a method whose declaration is a macro-body
            // literal has to be keyed by its resolved identifier and located from
            // that identifier's own token, or the open-buffer index disagrees with
            // the shard index for the same file.
            const auto* id_name = proto.name->as_if<IdentifierNameSyntax>();
            const auto name_tok = id_name ? id_name->identifier : proto.keyword;
            auto [ml, mc] = token_pos_line1_col0(sm, name_tok);
            entry.methods.push_back(MethodEntry{.name = id_name
                                                    ? std::string(id_name->identifier.valueText())
                                                    : node_text_raw(sm, *proto.name),
                                                .return_type = node_text_raw(sm, *proto.returnType),
                                                .params = proto.portList
                                                              ? trim_copy(proto.portList->toString())
                                                              : std::string{},
                                                .is_task = method->declaration->kind ==
                                                           SyntaxKind::TaskDeclaration,
                                                .file_id = resolver.for_declaration_token(index, sm, name_tok),
                                                .line = ml,
                                                .col = mc});
        } else if (const auto* proto_item = item->as_if<ClassMethodPrototypeSyntax>()) {
            // `extern function void bump(...);` declares a member too.  The shard
            // builder indexes these (see process_class in syntax_index.cpp); the
            // open-buffer index has to as well, or a class written in the extern
            // style loses every method the moment its file is opened.
            const auto& proto = *proto_item->prototype;
            const auto* id_name = proto.name->as_if<IdentifierNameSyntax>();
            const auto name_tok = id_name ? id_name->identifier : proto.keyword;
            auto [ml, mc] = token_pos_line1_col0(sm, name_tok);
            entry.methods.push_back(
                MethodEntry{.name = id_name ? std::string(id_name->identifier.valueText())
                                            : node_text_raw(sm, *proto.name),
                            .return_type = node_text_raw(sm, *proto.returnType),
                            .params = proto.portList ? trim_copy(proto.portList->toString())
                                                     : std::string{},
                            .is_task = proto.keyword.kind == slang::parsing::TokenKind::TaskKeyword,
                            .file_id = resolver.for_declaration_token(index, sm, name_tok),
                            .line = ml,
                            .col = mc});
        }
    }
    if (!entry.parent_scope.empty() && index.package_names.count(entry.parent_scope))
        index.package_class_by_scoped_name.try_emplace(
            package_scoped_key(entry.parent_scope, entry.name), index.classes.size());
    index.class_by_name.try_emplace(entry.name, index.classes.size());
    index.classes.push_back(std::move(entry));
}

void process_typedef(const TypedefDeclarationSyntax& td, SyntaxIndex& index,
                     SourceFileIdResolver& resolver, const slang::SourceManager& sm,
                     std::string parent_scope = {}, bool scoped_lookup) {
    TypedefEntry entry;
    entry.name = token_value_text(td.name);
    entry.parent_scope = std::move(parent_scope);
    entry.file_id = resolver.for_declaration_token(index, sm, td.name);
    auto [line, col] = token_pos_line1_col0(sm, td.name);
    entry.line = line;
    entry.col = col;
    if (const auto* enum_type = td.type->as_if<EnumTypeSyntax>()) {
        entry.is_enum = true;
        for (const auto* member : enum_type->members) {
            if (member) {
                auto [em_line, em_col] = token_pos_line1_col0(sm, member->name);
                entry.enum_members.push_back(EnumMemberEntry{
                    .name = token_value_text(member->name),
                    .file_id = resolver.for_declaration_token(index, sm, member->name),
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
                auto [fl, fc] = token_pos_line1_col0(sm, decl->name);
                entry.fields.push_back(FieldEntry{.name = token_value_text(decl->name),
                                                  .type = with_dims(sm, type, *decl),
                                                  .file_id = resolver.for_declaration_token(index, sm, decl->name),
                                                  .line = fl,
                                                  .col = fc});
            }
        }
    } else {
        entry.resolved = node_text_raw(sm, *td.type);
    }

    // Any `::`-addressable owner, matching the closed-file shard: a class-scoped
    // `type_id` is spelled once per class, so the bare-name table answers with
    // whichever class was seen first.  Module scope is excluded on purpose --
    // nothing can name `mod::t`.  `scoped_lookup` covers class members, whose
    // ClassEntry is not registered until its body has been walked.
    if (!entry.parent_scope.empty() &&
        (scoped_lookup || index.package_names.count(entry.parent_scope)))
        index.package_type_by_scoped_name.try_emplace(
            package_scoped_key(entry.parent_scope, entry.name), index.typedefs.size());
    index.typedef_by_name.try_emplace(entry.name, index.typedefs.size());
    index.typedefs.push_back(std::move(entry));
}

void process_module(const ModuleDeclarationSyntax& node, SyntaxIndex& index,
                    SourceFileIdResolver& resolver, const slang::SourceManager& sm,
                    std::string_view source) {
    ModuleEntry module;
    module.name = token_value_text(node.header->name);
    module.file_id = resolver.for_declaration_token(index, sm, node.header->name);
    auto [line, col] = token_pos_line1_col0(sm, node.header->name);
    module.line = line;
    module.col = col;
    fill_module_edit_ranges(module, *node.header, sm);

    if (node.header->parameters) {
        for (const auto* base : node.header->parameters->declarations) {
            // `parameter type foo_t = ...` is a separate syntax node; without
            // this branch it never reaches the open-buffer index either.
            if (const auto* type_param =
                    base ? base->as_if<TypeParameterDeclarationSyntax>() : nullptr) {
                const std::string direction = token_value_text(type_param->keyword);
                for (const auto* decl : type_param->declarators) {
                    if (!decl)
                        continue;
                    add_port(module, index, resolver, sm, decl->name, direction, "type", "type", {},
                             decl->assignment ? node_text_raw(sm, *decl->assignment->type)
                                              : std::string{});
                }
                continue;
            }
            const auto* param = base ? base->as_if<ParameterDeclarationSyntax>() : nullptr;
            if (!param)
                continue;
            const std::string direction = token_value_text(param->keyword);
            const std::string type = node_text_raw(sm, *param->type);
            for (const auto* decl : param->declarators) {
                if (!decl)
                    continue;
                std::string default_value;
                if (decl->initializer)
                    default_value = node_text_raw(sm, *decl->initializer->expr);
                add_port(module, index, resolver, sm, decl->name, direction, type, type, {},
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
                    add_port(module, index, resolver, sm, implicit->declarator->name,
                             port_direction(*implicit->header), type, decl_type, signal_type);
                } else if (const auto* explicit_port =
                               item ? item->as_if<ExplicitAnsiPortSyntax>() : nullptr) {
                    add_port(module, index, resolver, sm, explicit_port->name, token_value_text(explicit_port->direction),
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
                    add_port(module, index, resolver, sm, decl->name, direction, with_dims(sm, type, *decl),
                             with_dims(sm, port_decl_type(sm, *port_decl->header), *decl),
                             with_dims(sm, port_signal_decl_type(sm, *port_decl->header), *decl));
            }
        } else if (const auto* data = member->as_if<DataDeclarationSyntax>()) {
            // Rendered on the first declarator actually kept: a shared header's
            // declarations would otherwise pay type rendering once per including
            // file only to be dropped.  Mirrors the same branch in
            // syntax_index.cpp so an open buffer and its closed shard agree.
            std::optional<std::string> type;
            for (const auto* decl : data->declarators) {
                if (!decl)
                    continue;
                if (!resolver.wants_declaration(index, sm, decl->name))
                    continue;
                if (!type)
                    type = node_text_raw(sm, *data->type);
                add_value(index, resolver, sm, decl->name, with_dims(sm, *type, *decl), "variable",
                          module.name);
            }
        } else if (const auto* fn = member->as_if<FunctionDeclarationSyntax>()) {
            if (auto* value = add_value(index, resolver, sm, fn->prototype->keyword,
                                        node_text_raw(sm, *fn->prototype->returnType), "function",
                                        module.name))
                value->name = node_text_raw(sm, *fn->prototype->name);
        } else if (const auto* ps = member->as_if<ParameterDeclarationStatementSyntax>()) {
            // Body parameters (`localparam DEPTH = 1;`), as opposed to header
            // `#(...)` parameters handled above.  Mirrors the same branch in
            // syntax_index.cpp so open buffers and closed shards agree.
            if (const auto* param = ps->parameter->as_if<ParameterDeclarationSyntax>()) {
                std::optional<std::string> type;
                std::string kind;
                for (const auto* decl : param->declarators) {
                    if (!decl)
                        continue;
                    if (!resolver.wants_declaration(index, sm, decl->name))
                        continue;
                    if (!type) {
                        type = node_text_raw(sm, *param->type);
                        kind = token_value_text(param->keyword);
                    }
                    if (auto* value = add_value(index, resolver, sm, decl->name, *type, kind, module.name))
                        value->default_value =
                            decl->initializer ? node_text_raw(sm, *decl->initializer->expr)
                                              : std::string{};
                }
            }
        } else if (const auto* cls = member->as_if<ClassDeclarationSyntax>()) {
            // Module-scoped class declarations are valid SystemVerilog symbols.
            // Keep their parent scope so member-access lookup can distinguish
            // the compact identity `top::Packet` from a compilation-unit class
            // with the same spelling.
            process_class(*cls, index, resolver, sm, module.name);
        } else if (const auto* td = member->as_if<TypedefDeclarationSyntax>()) {
            // Module-scoped typedefs are especially important for aggregate
            // member lookup:
            //
            //     typedef struct packed { logic valid; } fifo_entry_t;
            //     fifo_entry_t fifo_entry;
            //     assign x = fifo_entry.valid;
            //
            // The dedicated member-access resolver identifies `fifo_entry` as
            // type `fifo_entry_t` and then searches indexed typedef fields.
            // Without this shard fact, it falls through to generic lookup.
            process_typedef(*td, index, resolver, sm, module.name);
        } else if (const auto* hierarchy = member->as_if<HierarchyInstantiationSyntax>()) {
            process_hierarchy(*hierarchy, index, resolver, sm, lines, module.name);
        } else if (is_generate_construct(*member)) {
            process_generate_instances(*member, index, resolver, sm, lines, module.name);
        } else if (const auto* modport = member->as_if<ModportDeclarationSyntax>()) {
            for (const auto* item : modport->items) {
                if (!item)
                    continue;
                auto [ml, mc] = token_pos_line1_col0(sm, item->name);
                module.modports.push_back(ModportEntry{.name = token_value_text(item->name),
                                                       .file_id = resolver.for_declaration_token(index, sm, item->name),
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

void process_package(const ModuleDeclarationSyntax& pkg, SyntaxIndex& index,
                     SourceFileIdResolver& resolver, const slang::SourceManager& sm) {
    ModuleEntry module;
    module.name = token_value_text(pkg.header->name);
    module.file_id = resolver.for_declaration_token(index, sm, pkg.header->name);
    auto [line, col] = token_pos_line1_col0(sm, pkg.header->name);
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
                index.package_value_by_scoped_name.try_emplace(
                    package_scoped_key(module.name, token_value_text(decl->name)),
                    index.values.size());
                add_value(index, resolver, sm, decl->name, with_dims(sm, type, *decl), "variable", module.name);
                index.package_symbols[module.name].push_back(token_value_text(decl->name));
            }
        } else if (const auto* ps = member->as_if<ParameterDeclarationStatementSyntax>()) {
            if (const auto* param = ps->parameter->as_if<ParameterDeclarationSyntax>()) {
                const auto type = node_text_raw(sm, *param->type);
                for (const auto* decl : param->declarators) {
                    if (!decl)
                        continue;
                    auto [pl, pc] = token_pos_line1_col0(sm, decl->name);
                    index.package_value_by_scoped_name.try_emplace(
                        package_scoped_key(module.name, token_value_text(decl->name)),
                        index.values.size());
                    index.values.push_back(ValueEntry{.name = token_value_text(decl->name),
                                                      .type = type,
                                                      .kind = token_value_text(param->keyword),
                                                      .parent_scope = module.name,
                                                      .default_value =
                                                          decl->initializer
                                                              ? node_text_raw(sm, *decl->initializer->expr)
                                                              : std::string{},
                                                      .file_id = resolver.for_declaration_token(index, sm, decl->name),
                                                      .line = pl,
                                                      .col = pc});
                    index.package_symbols[module.name].push_back(token_value_text(decl->name));
                }
            }
        } else if (const auto* fn = member->as_if<FunctionDeclarationSyntax>()) {
            // Mirrors the package branch in syntax_index.cpp.  The entry must
            // carry the declaration position: without it go-to-definition on an
            // imported package subroutine lands on line 0 / column 0 whenever
            // the package file happens to be open in the editor, because the
            // open buffer is answered from this shard instead of the
            // disk-backed one.
            const auto& proto = *fn->prototype;
            const auto* id_name = proto.name->as_if<IdentifierNameSyntax>();
            const auto name_tok = id_name ? id_name->identifier : proto.keyword;
            const auto name = id_name ? std::string(id_name->identifier.valueText())
                                      : node_text_raw(sm, *proto.name);
            auto [nl, nc] = token_pos_line1_col0(sm, name_tok);
            index.package_value_by_scoped_name.try_emplace(
                package_scoped_key(module.name, name), index.values.size());
            index.values.push_back(
                ValueEntry{.name = name,
                           .type = node_text_raw(sm, *proto.returnType),
                           .kind = token_value_text(proto.keyword),
                           .parent_scope = module.name,
                           .file_id = resolver.for_declaration_token(index, sm, name_tok),
                           .line = nl,
                           .col = nc,
                           .signature = make_subroutine_signature(proto, name, sm)});
            index.package_symbols[module.name].push_back(name);
        } else if (const auto* cls = member->as_if<ClassDeclarationSyntax>()) {
            process_class(*cls, index, resolver, sm, module.name);
            index.package_symbols[module.name].push_back(token_value_text(cls->name));
        } else if (const auto* td = member->as_if<TypedefDeclarationSyntax>()) {
            process_typedef(*td, index, resolver, sm, module.name);
            index.package_symbols[module.name].push_back(token_value_text(td->name));
            // Enum members are package members in their own right, and
            // syntax_index.cpp lists them here too.  Without them an open
            // package buffer cannot report that it owns `RED`.
            if (const auto* enum_type = td->type->as_if<EnumTypeSyntax>()) {
                for (const auto* enum_member : enum_type->members) {
                    if (enum_member)
                        index.package_symbols[module.name].push_back(
                            token_value_text(enum_member->name));
                }
            }
        }
    }

    index.module_by_name.try_emplace(module.name, index.modules.size());
    index.modules.push_back(std::move(module));
}

void collect_imports(const SyntaxNode& root, SyntaxIndex& index, SourceFileIdResolver& resolver,
                     const slang::SourceManager& sm) {
    struct Visitor : SyntaxVisitor<Visitor> {
        SyntaxIndex& index;
        SourceFileIdResolver& resolver;
        const slang::SourceManager& sm;
        explicit Visitor(SyntaxIndex& index, SourceFileIdResolver& resolver,
                         const slang::SourceManager& sm)
            : index(index), resolver(resolver), sm(sm) {}
        void handle(const PackageImportDeclarationSyntax& node) {
            auto [line, _] = token_pos_line1_col0(sm, node.keyword);
            for (const auto* item : node.items) {
                if (!item)
                    continue;
                ImportEntry entry;
                entry.package_name = token_value_text(item->package);
                entry.wildcard = item->item.kind == slang::parsing::TokenKind::Star;
                if (!entry.wildcard)
                    entry.symbol_name = token_value_text(item->item);
                entry.file_id = resolver.for_declaration_token(index, sm, item->package);
                entry.start_line = line;
                index.imports.push_back(std::move(entry));
            }
            visitDefault(node);
        }
    } visitor(index, resolver, sm);
    root.visit(visitor);
}

void collect_macros(const slang::syntax::SyntaxTree& tree, SyntaxIndex& index,
                    SourceFileIdResolver& resolver) {
    const auto& sm = tree.sourceManager();
    for (const auto* def : tree.getDefinedMacros()) {
        if (!def || !def->name || !def->name.location().valid() || !sm.isFileLoc(def->name.location()))
            continue;
        auto [line, _] = token_pos_line1_col0(sm, def->name);
        MacroEntry mac;
        mac.name = token_value_text(def->name);
        mac.file_id = resolver.for_declaration_token(index, sm, def->name);
        mac.line = line;
        if (def->formalArguments) {
            mac.is_function_like = true;
            for (const auto* arg : def->formalArguments->args) {
                if (arg)
                    mac.params.push_back(token_value_text(arg->name));
            }
        }
        index.macros.push_back(std::move(mac));
    }
}



/// Walk the live AST into a shard.
///
/// @p restrict_to_uri empty keeps every buffer the tree covers, which is what a
/// whole-file structural view wants.  Non-empty scopes the build to one file the
/// way SyntaxIndex::build() does for background shards: declarations from an
/// `include`d header survive only when this file mentions their name, and
/// occurrence tables cover this file alone.  The header's own shard is what
/// records the rest, once for the whole project instead of once per includer.
SyntaxIndex build_structural_index(const DocumentState& state, std::string_view restrict_to_uri) {
    SyntaxIndex index;
    if (!state.tree)
        return index;

    const auto& root = state.tree->root();
    const auto& sm = state.tree->sourceManager();
    SourceFileIdResolver resolver;

    // Built on the first declaration that actually comes from another file:
    // scanning the buffer costs more than it saves for a file that `include`s
    // nothing.  The views point into state.text and into macro body tokens, both
    // of which outlive this function.
    std::unordered_set<std::string_view> mentioned_names;
    bool mentions_ready = false;
    auto ensure_mentions = [&]() -> const std::unordered_set<std::string_view>* {
        if (!mentions_ready) {
            mentioned_names = collect_mentioned_names(state.text, *state.tree);
            mentions_ready = true;
        }
        return &mentioned_names;
    };
    if (!restrict_to_uri.empty()) {
        resolver.restrict_to_uri(std::string(restrict_to_uri));
        resolver.set_mentions_provider(ensure_mentions);
    }

    auto process_member = [&](const MemberSyntax& member) {
        if (const auto* mod = member.as_if<ModuleDeclarationSyntax>()) {
            if (member.kind == SyntaxKind::InterfaceDeclaration) {
                process_module(*mod, index, resolver, sm, state.text);
                index.interface_names.insert(token_value_text(mod->header->name));
            } else if (member.kind == SyntaxKind::PackageDeclaration) {
                process_package(*mod, index, resolver, sm);
            } else {
                process_module(*mod, index, resolver, sm, state.text);
            }
        } else if (const auto* cls = member.as_if<ClassDeclarationSyntax>())
            process_class(*cls, index, resolver, sm);
        else if (const auto* td = member.as_if<TypedefDeclarationSyntax>())
            process_typedef(*td, index, resolver, sm);
    };

    if (const auto* cu = root.as_if<CompilationUnitSyntax>()) {
        for (const auto* member : cu->members) {
            if (member)
                process_member(*member);
        }
    } else if (const auto* member = root.as_if<MemberSyntax>()) {
        process_member(*member);
    }

    collect_combined_occurrences(*state.tree, root, index, sm, restrict_to_uri,
                                 mentions_ready ? &mentioned_names : nullptr);
    index.include_dependencies = collect_include_dependency_uris(sm, state.uri);
    return index;
}

} // namespace

SyntaxIndex build_current_ast_structural_index(const DocumentState& state) {
    return build_structural_index(state, {});
}

const SyntaxIndex& get_structural_index(const DocumentState& state) {
    std::call_once(state.structural_index_once_, [&state] {
        state.structural_index_cache_ = build_current_ast_structural_index(state);
    });
    return state.structural_index_cache_;
}

SyntaxIndex build_dynamic_file_index(const DocumentState& state) {
    // Scoped to this file, matching the disk-backed shard the background indexer
    // builds for the same path (Analyzer::make_file_state_with_options passes
    // restrict_index_to_own_file).  Without that, opening a file swapped its lean
    // shard for one carrying every declaration and every identifier occurrence of
    // whatever it `include`s — a header shared by N files costing N copies of
    // itself, rebuilt from scratch on each edit.
    //
    // This is deliberately not the structural cache: that one is the unrestricted
    // whole-file view AutoWire, Connect, RTL-tree and documentSymbol ask for, and
    // it stays unrestricted.
    SyntaxIndex index = build_structural_index(state, state.uri);
    if (!state.tree)
        return index;

    SourceFileIdResolver resolver;

    collect_imports(state.tree->root(), index, resolver, state.tree->sourceManager());
    // Macros stay unscoped on purpose.  Header shards are built at Declarations
    // depth, which skips macros entirely, so this shard is the only place a
    // header's `define reaches macro completion from.
    collect_macros(*state.tree, index, resolver);
    // The structural walk above already recorded macro reference occurrences for
    // the open file.  `collect_macros()` only adds completion metadata
    // (MacroEntry), so do not add references a second time here.
    return index;
}

const SyntaxIndex& get_dynamic_index(const DocumentState& state) {
    std::call_once(state.dynamic_index_once_, [&state] {
        state.dynamic_index_cache_ = build_dynamic_file_index(state);
    });
    return state.dynamic_index_cache_;
}
