#include "document_symbols.hpp"
#include "../dynamic_file_index.hpp"
#include <unordered_map>

namespace {

int to_lsp_line(int one_based_line) { return one_based_line > 0 ? one_based_line - 1 : 0; }

lsDocumentSymbol make_symbol(const std::string& name, lsSymbolKind kind, int line, int col,
                             optional<std::string> detail = {}) {
    const int l = to_lsp_line(line);
    lsDocumentSymbol sym;
    sym.name = name;
    sym.kind = kind;
    sym.range.start = lsPosition(l, col);
    sym.range.end = lsPosition(l, col + (int)name.size());
    sym.selectionRange = sym.range;
    sym.detail = std::move(detail);
    return sym;
}

optional<std::string> opt_str(std::string text) {
    return text.empty() ? optional<std::string>{} : optional<std::string>(std::move(text));
}

void push_child(lsDocumentSymbol& parent, lsDocumentSymbol child) {
    if (!parent.children)
        parent.children = std::vector<lsDocumentSymbol>{};
    parent.children->push_back(std::move(child));
}

} // namespace

std::vector<lsDocumentSymbol> provide_document_symbols(const Analyzer& analyzer,
                                                        const lsDocumentSymbolParams& params) {
    std::vector<lsDocumentSymbol> result;

    const auto& uri = params.textDocument.uri.raw_uri_;
    auto state = analyzer.get_state(uri);
    if (!state || !state->tree)
        return result;

    const SyntaxIndex& index = get_structural_index(*state);

    // lsDocumentSymbol carries no URI, so every range it reports is read against
    // the requested document.  Index entries, however, record where a
    // declaration really came from: an `include`d header, or the macro body that
    // generated it (`uvm_object_utils` and friends).  Emitting those lines here
    // would place symbols past this file's end.  Keep only entries that belong
    // to this document; an unknown file_id means "current file".
    const auto in_document = [&](SourceFileID file_id) {
        const auto actual_uri = index.source_uri(file_id);
        return actual_uri.empty() || actual_uri == uri;
    };

    // Modules/interfaces/packages are always top-level containers.  Track
    // where each one landed in `result` so classes/typedefs declared inside
    // one (most commonly a package) can be nested under it below.
    std::unordered_map<std::string, size_t> container_index;
    for (const auto& mod : index.modules) {
        if (!in_document(mod.file_id))
            continue;
        lsSymbolKind kind = lsSymbolKind::Module;
        if (index.interface_names.count(mod.name))
            kind = lsSymbolKind::Interface;
        else if (index.package_names.count(mod.name))
            kind = lsSymbolKind::Package;

        lsDocumentSymbol node = make_symbol(mod.name, kind, mod.line, mod.col);

        for (const auto& p : mod.ports) {
            if (!in_document(p.file_id))
                continue;
            std::string detail = p.direction;
            if (!p.type.empty())
                detail += (detail.empty() ? "" : " ") + p.type;
            const bool is_param = p.direction == "parameter" || p.direction == "localparam";
            push_child(node, make_symbol(p.name,
                                         is_param ? lsSymbolKind::Constant : lsSymbolKind::Property,
                                         p.line, p.col, opt_str(std::move(detail))));
        }
        for (const auto& mp : mod.modports) {
            if (!in_document(mp.file_id))
                continue;
            push_child(node, make_symbol(mp.name, lsSymbolKind::Interface, mp.line, mp.col,
                                         optional<std::string>("modport")));
        }
        // Module-level variables/functions/tasks. Ports/parameters are
        // skipped here -- they're already covered above via mod.ports and
        // would otherwise be listed twice (process_module() files both).
        for (const auto& v : index.values) {
            if (v.parent_scope != mod.name || !in_document(v.file_id))
                continue;
            if (v.kind == "variable") {
                push_child(node, make_symbol(v.name, lsSymbolKind::Variable, v.line, v.col,
                                             opt_str(v.type)));
            } else if (v.kind == "function" || v.kind == "task") {
                push_child(node, make_symbol(v.name,
                                             v.kind == "task" ? lsSymbolKind::Method
                                                              : lsSymbolKind::Function,
                                             v.line, v.col, opt_str(v.type)));
            }
        }
        for (const auto& inst : index.instances) {
            if (inst.parent_module != mod.name || !in_document(inst.file_id))
                continue;
            push_child(node, make_symbol(inst.instance_name, lsSymbolKind::Object, inst.line, 0,
                                         opt_str(inst.module_name)));
        }

        container_index.emplace(mod.name, result.size());
        result.push_back(std::move(node));
    }

    // Classes nest under their declaring package/class when known, otherwise
    // sit at the top level (e.g. a class declared directly in the file).
    for (const auto& cls : index.classes) {
        if (!in_document(cls.file_id))
            continue;
        lsDocumentSymbol node = make_symbol(cls.name, lsSymbolKind::Class, cls.line, cls.col);
        for (const auto& f : cls.fields) {
            if (!in_document(f.file_id))
                continue;
            push_child(node, make_symbol(f.name, lsSymbolKind::Field, f.line, f.col, opt_str(f.type)));
        }
        for (const auto& m : cls.methods) {
            if (!in_document(m.file_id))
                continue;
            push_child(node, make_symbol(m.name, m.is_task ? lsSymbolKind::Method : lsSymbolKind::Function,
                                         m.line, m.col, opt_str(m.return_type)));
        }

        const auto it = container_index.find(cls.parent_scope);
        if (!cls.parent_scope.empty() && it != container_index.end())
            push_child(result[it->second], std::move(node));
        else
            result.push_back(std::move(node));
    }

    // Typedefs: same nesting rule as classes.
    for (const auto& td : index.typedefs) {
        if (!in_document(td.file_id))
            continue;
        const lsSymbolKind kind = td.is_struct ? lsSymbolKind::Struct
                                  : td.is_enum ? lsSymbolKind::Enum
                                               : lsSymbolKind::TypeAlias;
        lsDocumentSymbol node = make_symbol(td.name, kind, td.line, td.col, opt_str(td.resolved));
        if (td.is_enum) {
            for (const auto& em : td.enum_members) {
                if (!in_document(em.file_id))
                    continue;
                push_child(node, make_symbol(em.name, lsSymbolKind::EnumMember, em.line, em.col));
            }
        } else if (td.is_struct) {
            for (const auto& f : td.fields) {
                if (!in_document(f.file_id))
                    continue;
                push_child(node, make_symbol(f.name, lsSymbolKind::Field, f.line, f.col, opt_str(f.type)));
            }
        }

        const auto it = container_index.find(td.parent_scope);
        if (!td.parent_scope.empty() && it != container_index.end())
            push_child(result[it->second], std::move(node));
        else
            result.push_back(std::move(node));
    }

    return result;
}
