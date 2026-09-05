#include "document_symbols.hpp"
#include "../dynamic_file_index.hpp"
#include "../string_utils.hpp"
#include "../syntax_index_shared.hpp"
#include <functional>
#include <unordered_map>
#include <slang/syntax/AllSyntax.h>
#include <slang/syntax/SyntaxTree.h>
#include <slang/syntax/SyntaxVisitor.h>
#include <slang/text/SourceManager.h>

namespace {

int to_lsp_line(int one_based_line) { return one_based_line > 0 ? one_based_line - 1 : 0; }

// ── Source extents, read from the open buffer's AST ───────────────────────────
//
// The index records where a declaration *starts*, which is all a jump target
// needs.  An outline also needs where it ends: LSP gives `range` (the whole
// declaration, used for breadcrumbs and folding) and `selectionRange` (the name
// alone, used for the reveal).  Reporting the name span as both, which is what
// this file used to do, collapses the outline to a list of points.
//
// documentSymbol only ever answers for the open buffer, whose AST is
// authoritative, so these extents are derived here instead of being added to
// SyntaxIndex.  Nothing on the closed-file path wants them, and the project
// indexer should not pay for facts only the focused file uses.

/// A declaration written directly inside a labelled block.
///
/// These never reach the index: the closed-file shard skips them, and the
/// open-buffer index has no block-local pass at all -- adding one would put a
/// whole-tree walk on every edit.  documentSymbol already walks this file's AST
/// for extents, so the outline collects them here instead, at no extra cost and
/// with nothing added to the project index.
struct BlockDecl {
    std::string name;
    std::string type;
    int line{0}; // 1-based
    int col{0};  // 0-based
};

struct GenerateBlock {
    std::string name;
    // "generate" or "block" -- what the outline shows next to the name.  A named
    // `always_comb begin : proc` is a navigable scope in exactly the same way a
    // labelled generate is, and it holds declarations of its own.
    std::string detail{"generate"};
    int line{0}; // 1-based
    int col{0};  // 0-based
    int end_line{0};
    int end_col{0};
    int parent{-1}; // index into the module's block list
    std::vector<BlockDecl> decls;
};

struct Extent {
    int end_line{0}; // 1-based, 0 when unknown
    int end_col{0};  // 0-based exclusive
};

struct ModuleShape {
    Extent extent;
    std::vector<GenerateBlock> blocks;
};

struct DocumentShape {
    std::unordered_map<std::string, ModuleShape> modules;
    std::unordered_map<std::string, Extent> classes;
    // Subroutine bodies, keyed by the 1-based line the declaration starts on.
    // Modules and classes already report a full range; a function reported only
    // the line its name sits on, so folding and breadcrumbs behaved differently
    // for the two.  Keying on the line avoids inventing a unique name for a
    // method that several classes declare (`new`, `run_phase`, ...).
    std::unordered_map<int, Extent> subroutines;
};

struct ShapeVisitor : slang::syntax::SyntaxVisitor<ShapeVisitor> {
    const slang::SourceManager& sm;
    const std::string& uri;
    DocumentShape& out;
    ModuleShape* current{nullptr};
    std::vector<int> block_stack;

    ShapeVisitor(const slang::SourceManager& sm, const std::string& uri, DocumentShape& out)
        : sm(sm), uri(uri), out(out) {}

    // A tree may span several files through `include; only declarations written
    // in the requested document have ranges this response can report.
    bool in_document(slang::SourceLocation loc) const {
        const auto actual = uri_from_source_location(sm, loc);
        return actual.empty() || actual == uri;
    }

    Extent extent_of(const slang::syntax::SyntaxNode& node) const {
        const auto end = node.sourceRange().end();
        if (!end.valid())
            return {};
        const auto line = sm.getLineNumber(end);
        const auto col = sm.getColumnNumber(end);
        return Extent{(int)line, col > 0 ? (int)col - 1 : 0};
    }

    void handle(const slang::syntax::ModuleDeclarationSyntax& node) {
        if (!in_document(node.header->name.location())) {
            visitDefault(node);
            return;
        }
        auto& shape = out.modules[std::string(node.header->name.valueText())];
        shape.extent = extent_of(node);
        ModuleShape* previous = current;
        current = &shape;
        visitDefault(node);
        current = previous;
    }

    void handle(const slang::syntax::ClassDeclarationSyntax& node) {
        if (in_document(node.name.location()))
            out.classes[std::string(node.name.valueText())] = extent_of(node);
        visitDefault(node);
    }

    void handle(const slang::syntax::DataDeclarationSyntax& node) {
        if (current && !block_stack.empty()) {
            auto& block = current->blocks[(size_t)block_stack.back()];
            const std::string type = trim_copy(std::string(node.type->toString()));
            for (const auto* decl : node.declarators) {
                if (!decl || !decl->name || !in_document(decl->name.location()))
                    continue;
                const auto col = sm.getColumnNumber(decl->name.location());
                block.decls.push_back(BlockDecl{std::string(decl->name.valueText()), type,
                                                (int)sm.getLineNumber(decl->name.location()),
                                                col > 0 ? (int)col - 1 : 0});
            }
        }
        visitDefault(node);
    }

    void handle(const slang::syntax::FunctionDeclarationSyntax& node) {
        const auto first = node.getFirstToken();
        if (first && in_document(first.location()))
            out.subroutines[(int)sm.getLineNumber(first.location())] = extent_of(node);
        visitDefault(node);
    }

    /// Record a labelled scope and walk its body inside it.
    ///
    /// Returns false when the node is not something the outline nests under, in
    /// which case the caller just walks the body normally.
    bool enter_labelled_block(const slang::syntax::SyntaxNode& node,
                              slang::SourceLocation begin, std::string label,
                              std::string detail) {
        if (!current || label.empty() || !in_document(begin))
            return false;

        const auto extent = extent_of(node);
        GenerateBlock block;
        block.name = std::move(label);
        block.detail = std::move(detail);
        block.line = (int)sm.getLineNumber(begin);
        const auto col = sm.getColumnNumber(begin);
        block.col = col > 0 ? (int)col - 1 : 0;
        block.end_line = extent.end_line;
        block.end_col = extent.end_col;
        block.parent = block_stack.empty() ? -1 : block_stack.back();

        block_stack.push_back((int)current->blocks.size());
        current->blocks.push_back(std::move(block));
        return true;
    }

    void handle(const slang::syntax::GenerateBlockSyntax& node) {
        // Only a labelled block is a scope a reader navigates by; an unnamed
        // one would clutter the outline with `genblk` nodes the source never
        // spells.
        const auto* name_clause = node.beginName ? node.beginName : node.endName;
        std::string label =
            name_clause ? std::string(name_clause->name.valueText()) : std::string{};
        if (label.empty() && node.label)
            label = std::string(node.label->name.valueText());

        const bool entered =
            enter_labelled_block(node, node.begin.location(), std::move(label), "generate");
        visitDefault(node);
        if (entered)
            block_stack.pop_back();
    }

    void handle(const slang::syntax::BlockStatementSyntax& node) {
        std::string label = node.blockName ? std::string(node.blockName->name.valueText())
                                           : std::string{};
        if (label.empty() && node.label)
            label = std::string(node.label->name.valueText());

        const bool entered =
            enter_labelled_block(node, node.begin.location(), std::move(label), "block");
        visitDefault(node);
        if (entered)
            block_stack.pop_back();
    }
};

// The innermost generate block whose lines cover @p line, or -1 for none.
int block_containing(const std::vector<GenerateBlock>& blocks, int line) {
    int best = -1;
    for (size_t i = 0; i < blocks.size(); ++i) {
        const auto& b = blocks[i];
        if (b.end_line <= 0 || line < b.line || line > b.end_line)
            continue;
        if (best < 0 || b.line >= blocks[(size_t)best].line)
            best = (int)i;
    }
    return best;
}

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

// Widen `range` to the whole declaration, leaving `selectionRange` on the name.
// LSP requires range to contain selectionRange, so a range that would not is
// dropped rather than reported.
void set_extent(lsDocumentSymbol& sym, const Extent& extent) {
    if (extent.end_line <= 0)
        return;
    const int end_line = to_lsp_line(extent.end_line);
    if (end_line < sym.range.end.line ||
        (end_line == sym.range.end.line && extent.end_col < sym.range.end.character))
        return;
    sym.range.end = lsPosition(end_line, extent.end_col);
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

    DocumentShape shape;
    ShapeVisitor shape_visitor(state->tree->sourceManager(), uri, shape);
    state->tree->root().visit(shape_visitor);

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
    // one (most commonly a package) can be nested under it below.  Classes are
    // registered the same way once they are placed, so a class-scoped typedef
    // reaches its class instead of falling out to the top level.
    struct Locator {
        size_t top{0};
        // Index into the top-level node's children, or -1 for the node itself.
        int child{-1};
    };
    std::unordered_map<std::string, Locator> container_index;
    const auto container_node = [&](const Locator& loc) -> lsDocumentSymbol& {
        lsDocumentSymbol& top = result[loc.top];
        if (loc.child < 0)
            return top;
        return (*top.children)[(size_t)loc.child];
    };
    for (const auto& mod : index.modules) {
        if (!in_document(mod.file_id))
            continue;
        lsSymbolKind kind = lsSymbolKind::Module;
        if (index.interface_names.count(mod.name))
            kind = lsSymbolKind::Interface;
        else if (index.package_names.count(mod.name))
            kind = lsSymbolKind::Package;

        lsDocumentSymbol node = make_symbol(mod.name, kind, mod.line, mod.col);

        // A labelled generate block is a named scope holding declarations, so
        // it nests in the outline the way a package or class does.  Everything
        // declared between its `begin` and `end` lines belongs under it.
        const auto shape_it = shape.modules.find(mod.name);
        const ModuleShape* module_shape = shape_it == shape.modules.end() ? nullptr
                                                                         : &shape_it->second;
        if (module_shape)
            set_extent(node, module_shape->extent);

        std::vector<lsDocumentSymbol> block_nodes;
        std::vector<std::vector<int>> block_children;
        std::vector<int> block_roots;
        if (module_shape) {
            const auto& blocks = module_shape->blocks;
            block_children.resize(blocks.size());
            for (size_t i = 0; i < blocks.size(); ++i) {
                const auto& b = blocks[i];
                lsDocumentSymbol block_node =
                    make_symbol(b.name, lsSymbolKind::Namespace, b.line, b.col,
                                optional<std::string>(b.detail));
                set_extent(block_node, Extent{b.end_line, b.end_col});
                for (const auto& d : b.decls)
                    push_child(block_node, make_symbol(d.name, lsSymbolKind::Variable, d.line,
                                                       d.col, opt_str(d.type)));
                block_nodes.push_back(std::move(block_node));
                if (b.parent < 0)
                    block_roots.push_back((int)i);
                else
                    block_children[(size_t)b.parent].push_back((int)i);
            }
        }
        const auto target = [&](int line) -> lsDocumentSymbol& {
            const int block = module_shape ? block_containing(module_shape->blocks, line) : -1;
            return block < 0 ? node : block_nodes[(size_t)block];
        };

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
                push_child(target(v.line), make_symbol(v.name, lsSymbolKind::Variable, v.line,
                                                       v.col, opt_str(v.type)));
            } else if (v.kind == "parameter" || v.kind == "localparam") {
                // Parameter *ports* are already reported from mod.ports above.
                if (mod.port_by_name.count(v.name))
                    continue;
                std::string detail = v.type;
                if (!v.default_value.empty())
                    detail += (detail.empty() ? "= " : " = ") + v.default_value;
                push_child(target(v.line), make_symbol(v.name, lsSymbolKind::Constant, v.line,
                                                       v.col, opt_str(std::move(detail))));
            } else if (v.kind == "function" || v.kind == "task") {
                auto symbol = make_symbol(v.name,
                                          v.kind == "task" ? lsSymbolKind::Method
                                                           : lsSymbolKind::Function,
                                          v.line, v.col, opt_str(v.type));
                if (const auto extent = shape.subroutines.find(v.line);
                    extent != shape.subroutines.end())
                    set_extent(symbol, extent->second);
                push_child(target(v.line), std::move(symbol));
            }
        }
        for (const auto& inst : index.instances) {
            if (inst.parent_module != mod.name || !in_document(inst.file_id))
                continue;
            push_child(target(inst.line), make_symbol(inst.instance_name, lsSymbolKind::Object,
                                                      inst.line, 0, opt_str(inst.module_name)));
        }

        // Blocks are collected in source order, so a parent always precedes its
        // children; assembling from the roots down keeps sibling order intact.
        if (!block_nodes.empty()) {
            std::function<lsDocumentSymbol(int)> collect = [&](int i) {
                lsDocumentSymbol block_node = std::move(block_nodes[(size_t)i]);
                for (int child : block_children[(size_t)i])
                    push_child(block_node, collect(child));
                return block_node;
            };
            for (int root : block_roots)
                push_child(node, collect(root));
        }

        container_index.emplace(mod.name, Locator{result.size(), -1});
        result.push_back(std::move(node));
    }

    // Classes nest under their declaring package/class when known, otherwise
    // sit at the top level (e.g. a class declared directly in the file).
    for (const auto& cls : index.classes) {
        if (!in_document(cls.file_id))
            continue;
        lsDocumentSymbol node = make_symbol(cls.name, lsSymbolKind::Class, cls.line, cls.col);
        if (const auto extent = shape.classes.find(cls.name); extent != shape.classes.end())
            set_extent(node, extent->second);
        for (const auto& f : cls.fields) {
            if (!in_document(f.file_id))
                continue;
            push_child(node, make_symbol(f.name, lsSymbolKind::Field, f.line, f.col, opt_str(f.type)));
        }
        for (const auto& m : cls.methods) {
            if (!in_document(m.file_id))
                continue;
            auto method = make_symbol(m.name,
                                      m.is_task ? lsSymbolKind::Method : lsSymbolKind::Function,
                                      m.line, m.col, opt_str(m.return_type));
            if (const auto extent = shape.subroutines.find(m.line);
                extent != shape.subroutines.end())
                set_extent(method, extent->second);
            push_child(node, std::move(method));
        }

        const auto it = container_index.find(cls.parent_scope);
        if (!cls.parent_scope.empty() && it != container_index.end()) {
            const Locator parent = it->second;
            lsDocumentSymbol& target = container_node(parent);
            push_child(target, std::move(node));
            // Only a top-level container's direct children are addressable, which
            // is as deep as class nesting goes in the shapes this index records.
            if (parent.child < 0)
                container_index.emplace(cls.name,
                                        Locator{parent.top, (int)target.children->size() - 1});
        } else {
            container_index.emplace(cls.name, Locator{result.size(), -1});
            result.push_back(std::move(node));
        }
    }

    // Typedefs: same nesting rule as classes.
    for (const auto& td : index.typedefs) {
        if (!in_document(td.file_id))
            continue;
        // lspcpp's TypeAlias is 252, outside the 1..26 range LSP defines for
        // SymbolKind, so a client that validates the enum drops the symbol.
        const lsSymbolKind kind = td.is_struct ? lsSymbolKind::Struct
                                  : td.is_enum ? lsSymbolKind::Enum
                                               : lsSymbolKind::TypeParameter;
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
            push_child(container_node(it->second), std::move(node));
        else
            result.push_back(std::move(node));
    }

    return result;
}
