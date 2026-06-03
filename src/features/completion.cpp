#include "completion.hpp"
#include "../syntax_index.hpp"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <slang/parsing/TokenKind.h>
#include <slang/syntax/AllSyntax.h>
#include <slang/syntax/SyntaxTree.h>
#include <slang/syntax/SyntaxVisitor.h>
#include <string>
#include <string_view>
#include <unordered_set>

// ── Text helpers ──────────────────────────────────────────────────────────────

static bool is_ident_char(char c) {
    return std::isalnum((unsigned char)c) || c == '_' || c == '$';
}

// Convert 0-based LSP (line, character) to byte offset in text.
static size_t position_to_offset(const std::string& text, int line, int col) {
    int cur_line = 0;
    size_t i = 0;
    while (i < text.size() && cur_line < line) {
        if (text[i] == '\n') ++cur_line;
        ++i;
    }
    return std::min(i + (size_t)std::max(0, col), text.size());
}

// Read an identifier backwards from pos. Returns the word in forward order;
// updates pos to the new start (before the word).
static std::string backward_read_word(const std::string& text, size_t& pos) {
    const size_t end = pos;
    while (pos > 0 && is_ident_char(text[pos - 1]))
        --pos;
    return text.substr(pos, end - pos);
}

static void backward_skip_ws(const std::string& text, size_t& pos) {
    while (pos > 0 && std::isspace((unsigned char)text[pos - 1]))
        --pos;
}

// Read the symbol that appears immediately before a SystemVerilog scope
// operator (`::`).
//
// The simple case is package scope:
//   uvm_pkg::|
//          ^^ pos points just before the first ':', so the answer is uvm_pkg.
//
// A class scope expression can be parameterized:
//   uvm_config_db#(uvm_object)::|
//                            ^^ skip the balanced #( ... ) suffix first,
//                               then read the base class name.
//
// We intentionally return only the base identifier.  The current syntax index
// stores class declarations by un-specialized name (`uvm_config_db`) rather
// than by every possible parameter specialization, which is exactly what LSP
// completion needs for static members.
static std::string syntax_scope_base_name(const slang::syntax::NameSyntax& name) {
    using namespace slang::syntax;

    // Package names and ordinary class names:
    //   uvm_pkg::
    //   my_class::
    if (const auto* id = name.as_if<IdentifierNameSyntax>())
        return std::string(id->identifier.valueText());

    // Parameterized class names:
    //   uvm_config_db#(uvm_object)::
    //
    // Slang parses the whole left side as ClassNameSyntax, so extracting the
    // `identifier` token gives the unspecialized class declaration name that
    // our SyntaxIndex stores (`uvm_config_db`).
    if (const auto* cls = name.as_if<ClassNameSyntax>())
        return std::string(cls->identifier.valueText());

    // System / keyword / selected names are not the primary target here, but
    // they are real NameSyntax forms. Returning their syntax-owned token text
    // keeps this helper syntactic instead of re-parsing the source string.
    if (const auto* sys = name.as_if<SystemNameSyntax>())
        return std::string(sys->systemIdentifier.valueText());
    if (const auto* kw = name.as_if<KeywordNameSyntax>())
        return std::string(kw->keyword.valueText());
    if (const auto* sel = name.as_if<IdentifierSelectNameSyntax>())
        return std::string(sel->identifier.valueText());

    // For nested scope forms, e.g. pkg::cls::, the current completion provider
    // can only consume one scope key. The useful base for static member lookup
    // is the right-most name of the already-parsed left side (`cls`).
    if (const auto* scoped = name.as_if<ScopedNameSyntax>())
        return syntax_scope_base_name(*scoped->right);

    return {};
}

static std::optional<std::string>
syntax_scope_base_before_double_colon(const slang::syntax::SyntaxTree& tree,
                                      size_t cursor_offset) {
    using namespace slang;
    using namespace slang::syntax;

    struct SeparatorVisitor : public SyntaxVisitor<SeparatorVisitor> {
        size_t cursor_offset;
        size_t best_start{0};
        size_t best_end{0};

        explicit SeparatorVisitor(size_t cursor_offset) : cursor_offset(cursor_offset) {}

        void visitToken(slang::parsing::Token token) {
            if (token && !token.isMissing() &&
                token.kind == parsing::TokenKind::DoubleColon &&
                token.location().valid()) {
                const size_t start = token.location().offset();
                const size_t end = start + token.rawText().size();
                if (end == cursor_offset && end >= best_end) {
                    best_start = start;
                    best_end = end;
                }
            }
        }
    };

    SeparatorVisitor separator(cursor_offset);
    tree.root().visit(separator);
    if (separator.best_end == 0)
        return std::nullopt;

    struct NameVisitor : public SyntaxVisitor<NameVisitor> {
        size_t separator_start;
        size_t best_name_end{0};
        std::optional<std::string> result;

        explicit NameVisitor(size_t separator_start) : separator_start(separator_start) {}

        void consider(const NameSyntax& node) {
            const auto range = node.sourceRange();
            if (!range.start().valid() || !range.end().valid())
                return;

            const size_t end = range.end().offset();
            if (end <= separator_start && end >= best_name_end) {
                auto base = syntax_scope_base_name(node);
                if (!base.empty()) {
                    best_name_end = end;
                    result = std::move(base);
                }
            }
        }

        void handle(const IdentifierNameSyntax& node) {
            consider(node);
            visitDefault(node);
        }

        void handle(const ClassNameSyntax& node) {
            consider(node);
            visitDefault(node);
        }

        void handle(const ScopedNameSyntax& node) {
            // ScopedNameSyntax handles ordinary pkg::name expressions.  The
            // generic nearest-name pass is still useful because incomplete
            // parameterized static scopes can be parsed as a ClassNameSyntax
            // followed by a DoubleColon token rather than as a ScopedNameSyntax.
            consider(node);

            const auto sep = node.separator;
            if (sep && !sep.isMissing() &&
                sep.kind == parsing::TokenKind::DoubleColon &&
                sep.location().valid()) {
                const size_t start = sep.location().offset();
                if (start == separator_start) {
                    auto base = syntax_scope_base_name(*node.left);
                    if (!base.empty()) {
                        best_name_end = separator_start;
                        result = std::move(base);
                    }
                }
            }

            visitDefault(node);
        }

        void handle(const SystemNameSyntax& node) {
            consider(node);
            visitDefault(node);
        }

        void handle(const KeywordNameSyntax& node) {
            consider(node);
            visitDefault(node);
        }

        void handle(const IdentifierSelectNameSyntax& node) {
            consider(node);
            visitDefault(node);
        }
    };

    NameVisitor names(separator.best_start);
    tree.root().visit(names);
    return names.result;
}

static std::string trim_completion_copy(std::string text) {
    auto first =
        std::find_if_not(text.begin(), text.end(), [](unsigned char c) { return std::isspace(c); });
    auto last = std::find_if_not(text.rbegin(), text.rend(), [](unsigned char c) {
                    return std::isspace(c);
                }).base();
    if (first >= last)
        return {};
    return std::string(first, last);
}

// Find the byte offset of the '(' that opens the argument list enclosing
// cursor_offset (paren depth 0 when scanning backward). Returns text.size()
// if not found.
static size_t find_opening_paren(const std::string& text, size_t cursor_offset) {
    if (cursor_offset == 0) return text.size();
    int depth = 0;
    size_t pos = cursor_offset;
    while (pos > 0) {
        --pos;
        const char c = text[pos];
        if (c == ')') ++depth;
        else if (c == '(') {
            if (depth == 0) return pos;
            --depth;
        }
    }
    return text.size();
}

static bool paren_is_event_control(const std::string& text, size_t open) {
    size_t pos = open;
    backward_skip_ws(text, pos);
    return pos > 0 && text[pos - 1] == '@';
}

// Given the offset of a '.' that triggered NamedPort/Parameter context, scan
// backward to find the enclosing module name (the type identifier in a
// hierarchy instantiation). Returns nullopt when not determinable.
static std::optional<std::string> find_enclosing_instance(const std::string& text,
                                                            size_t dot_offset) {
    // Find the '(' opening our current argument list
    const size_t paren = find_opening_paren(text, dot_offset);
    if (paren >= text.size()) return std::nullopt;

    size_t pos = paren; // just before '('
    backward_skip_ws(text, pos);

    // Skip a '#(...)' parameter block if present (for port list inside an
    // already-parameterized instantiation, e.g. foo #(.W(8)) u0 (.clk|))
    if (pos > 0 && text[pos - 1] == ')') {
        --pos; // consume ')'
        const size_t inner = find_opening_paren(text, pos + 1);
        if (inner < text.size()) {
            pos = inner;
            if (pos > 0 && text[pos - 1] == '#') --pos;
        }
        backward_skip_ws(text, pos);
    }

    // Skip instance name
    if (pos == 0 || !is_ident_char(text[pos - 1])) return std::nullopt;
    while (pos > 0 && is_ident_char(text[pos - 1])) --pos;
    backward_skip_ws(text, pos);

    // Read module/type name
    if (pos == 0 || !is_ident_char(text[pos - 1])) return std::nullopt;
    const size_t mod_end = pos;
    while (pos > 0 && is_ident_char(text[pos - 1])) --pos;
    return text.substr(pos, mod_end - pos);
}

// Prefix / fuzzy match score. Returns 0 when there is no match.
// Higher score = better match.
static int prefix_score(std::string_view candidate, std::string_view pre) {
    if (pre.empty()) return 50;

    // Exact case-insensitive prefix match
    if (candidate.size() >= pre.size()) {
        bool ok = true;
        for (size_t i = 0; i < pre.size(); ++i) {
            if (std::tolower((unsigned char)candidate[i]) !=
                std::tolower((unsigned char)pre[i])) {
                ok = false;
                break;
            }
        }
        if (ok) {
            // Exact label == prefix is highest; longer candidate = slightly lower
            return 100 - (int)std::min((size_t)49, candidate.size() - pre.size());
        }
    }

    // Fuzzy: all prefix chars appear in order inside candidate
    size_t pi = 0;
    for (size_t ci = 0; ci < candidate.size() && pi < pre.size(); ++ci) {
        if (std::tolower((unsigned char)candidate[ci]) ==
            std::tolower((unsigned char)pre[pi]))
            ++pi;
    }
    return (pi == pre.size()) ? 30 : 0;
}

static std::string infer_current_scope(const SyntaxIndex& index, int line) {
    const int one_based = line + 1;
    std::string best;
    int best_line = -1;
    for (const auto& m : index.modules) {
        if (m.line > 0 && m.line <= one_based && m.line > best_line) {
            best = m.name;
            best_line = m.line;
        }
    }
    return best;
}

static bool range_contains_offset(const slang::SourceRange& range, size_t offset) {
    if (!range.start().valid() || !range.end().valid())
        return false;
    return range.start().offset() <= offset && offset <= range.end().offset();
}

static KeywordContextKind infer_keyword_context(const DocumentState& state, size_t offset) {
    if (!state.tree)
        return KeywordContextKind::General;

    using namespace slang::syntax;

    struct Visitor : public SyntaxVisitor<Visitor> {
        size_t offset;
        KeywordContextKind result{KeywordContextKind::General};
        size_t best_width{SIZE_MAX};

        explicit Visitor(size_t offset) : offset(offset) {}

        void consider(const SyntaxNode& node, KeywordContextKind kind) {
            const auto range = node.sourceRange();
            if (!range_contains_offset(range, offset))
                return;
            const size_t width = range.end().offset() - range.start().offset();
            if (width <= best_width) {
                best_width = width;
                result = kind;
            }
        }

        void handle(const ClassDeclarationSyntax& node) {
            consider(node, KeywordContextKind::Class);
            visitDefault(node);
        }

        void handle(const CovergroupDeclarationSyntax& node) {
            consider(node, KeywordContextKind::Covergroup);
            visitDefault(node);
        }

        void handle(const StatementSyntax& node) {
            consider(node, KeywordContextKind::Procedural);
            visitDefault(node);
        }

        void handle(const ModuleDeclarationSyntax& node) {
            consider(node, KeywordContextKind::ModuleItem);
            visitDefault(node);
        }
    };

    Visitor visitor(offset);
    state.tree->root().visit(visitor);
    return visitor.result;
}

static std::optional<std::string> type_of_value(const SyntaxIndex& index,
                                                 const std::string& scope,
                                                 const std::string& name) {
    for (const auto& v : index.values) {
        if (v.name == name && !scope.empty() && v.parent_scope == scope && !v.type.empty())
            return v.type;
    }
    for (const auto& v : index.values) {
        if (v.name == name && !v.type.empty())
            return v.type;
    }
    return std::nullopt;
}

static std::string completion_base_type_name(std::string type) {
    type = trim_completion_copy(std::move(type));
    if (type.empty())
        return {};
    const size_t paren = type.find("#(");
    if (paren != std::string::npos)
        type.erase(paren);
    type = trim_completion_copy(std::move(type));
    size_t end = 0;
    while (end < type.size() && is_ident_char(type[end]))
        ++end;
    return type.substr(0, end);
}

static std::string infer_assignment_lhs_type(const std::string& text, size_t offset,
                                             const SyntaxIndex& index,
                                             const std::string& scope) {
    size_t pos = offset;
    backward_skip_ws(text, pos);
    if (pos == 0)
        return {};

    // Recognize the common RHS-expression completion point:
    //     lhs = |
    //     lhs <= |
    //     lhs = pre|
    // Prefix characters have already been left of the cursor in `offset`, so
    // the operator should be immediately before whitespace.
    if (text[pos - 1] == '=') {
        --pos;
        if (pos > 0 && text[pos - 1] == '<')
            --pos;
    } else {
        return {};
    }

    backward_skip_ws(text, pos);
    const std::string lhs = backward_read_word(text, pos);
    if (lhs.empty())
        return {};

    if (auto type = type_of_value(index, scope, lhs))
        return trim_completion_copy(*type);
    return {};
}

static std::unordered_set<std::string> enum_members_for_type(const SyntaxIndex& index,
                                                             const std::string& type) {
    std::unordered_set<std::string> members;
    if (type.empty())
        return members;
    for (const auto& t : index.typedefs) {
        if (t.name == type && t.is_enum) {
            for (const auto& em : t.enum_members)
                members.insert(em.name);
            break;
        }
    }
    return members;
}

static std::unordered_set<std::string> value_names_for_type(const SyntaxIndex& index,
                                                            const CompletionContext& ctx) {
    std::unordered_set<std::string> names;
    if (ctx.expected_type.empty())
        return names;

    const std::string expected = completion_base_type_name(ctx.expected_type);
    if (expected.empty())
        return names;

    for (const auto& v : index.values) {
        if (v.name.empty() || v.type.empty())
            continue;
        if (!ctx.current_scope_name.empty() && !v.parent_scope.empty() &&
            v.parent_scope != ctx.current_scope_name &&
            !index.package_names.count(v.parent_scope))
            continue;

        if (completion_base_type_name(v.type) == expected)
            names.insert(v.name);
    }
    return names;
}

// ── Item construction helper ──────────────────────────────────────────────────

static lsCompletionItem make_item(std::string label, lsCompletionItemKind kind) {
    lsCompletionItem item;
    item.label = std::move(label);
    item.kind = optional<lsCompletionItemKind>(kind);
    return item;
}

// ── Providers ─────────────────────────────────────────────────────────────────

// KeywordProvider: comprehensive context-aware SystemVerilog keyword list.
class KeywordProvider : public CompletionProvider {
  public:
    bool accepts(const CompletionContext& ctx) const override {
        return ctx.kind == CompletionContextKind::Identifier ||
               ctx.kind == CompletionContextKind::Unknown;
    }

    std::vector<lsCompletionItem> provide(const CompletionContext& ctx,
                                           const SyntaxIndex& /*index*/,
                                           const CancellationToken& /*tok*/) const override {
        // clang-format off
        static const char* kGeneral[] = {
            "module","interface","package","class","program","primitive",
            "typedef","struct","union","enum","import",
            "logic","wire","reg","bit","byte","shortint","int","longint",
            "integer","real","realtime","time","shortreal","string","chandle",
            "event","void","signed","unsigned","packed","unpacked",
            nullptr
        };
        static const char* kModuleItem[] = {
            "assign","always_comb","always_ff","always_latch","always",
            "initial","final","generate","endgenerate","endmodule",
            "function","task","typedef","class","covergroup","property",
            "sequence","clocking","parameter","localparam","genvar",
            "logic","wire","import","export",
            nullptr
        };
        static const char* kProcedural[] = {
            "if","else","case","casez","casex","endcase","unique","unique0",
            "priority","for","foreach","while","do","repeat","forever",
            "return","break","continue","disable","begin","end","fork",
            "join","join_any","join_none","assert","assume","cover",
            "wait","@(posedge","@(negedge",
            "$display","$write","$finish","$stop","$fatal","$error",
            "$warning","$info","$cast","$bits","$size","$signed","$unsigned",
            nullptr
        };
        static const char* kClass[] = {
            "function","task","constraint","covergroup","rand","randc",
            "static","virtual","local","protected","extern","pure",
            "typedef","class","extends","implements","new","this","super",
            "endclass",
            nullptr
        };
        static const char* kCovergroup[] = {
            "coverpoint","cross","bins","illegal_bins","ignore_bins",
            "option","type_option","with","iff","endgroup",
            nullptr
        };
        // clang-format on
        const char** keywords = kGeneral;
        switch (ctx.keyword_context) {
        case KeywordContextKind::ModuleItem:
            keywords = kModuleItem;
            break;
        case KeywordContextKind::Procedural:
            keywords = kProcedural;
            break;
        case KeywordContextKind::Class:
            keywords = kClass;
            break;
        case KeywordContextKind::Covergroup:
            keywords = kCovergroup;
            break;
        case KeywordContextKind::General:
            keywords = kGeneral;
            break;
        }
        std::vector<lsCompletionItem> items;
        for (int i = 0; keywords[i]; ++i)
            items.push_back(make_item(keywords[i], lsCompletionItemKind::Keyword));
        return items;
    }
};

// IdentifierProvider: modules, classes, interfaces, typedefs, ports, macros.
class IdentifierProvider : public CompletionProvider {
  public:
    bool accepts(const CompletionContext& ctx) const override {
        return ctx.kind == CompletionContextKind::Identifier ||
               ctx.kind == CompletionContextKind::Unknown;
    }

    std::vector<lsCompletionItem> provide(const CompletionContext& ctx,
                                           const SyntaxIndex& index,
                                           const CancellationToken& tok) const override {
        std::vector<lsCompletionItem> items;

        for (const auto& v : index.values) {
            if (tok.cancelled) throw CompletionCancelled{};
            // Best-effort visibility: prefer current lexical module/package
            // symbols, but keep package/global values visible until full import
            // and nested-scope resolution is implemented.
            if (!ctx.current_scope_name.empty() && !v.parent_scope.empty() &&
                v.parent_scope != ctx.current_scope_name &&
                !index.package_names.count(v.parent_scope))
                continue;

            auto item = make_item(v.name,
                                  (v.kind == "function") ? lsCompletionItemKind::Function
                                  : (v.kind == "task")   ? lsCompletionItemKind::Method
                                                        : lsCompletionItemKind::Variable);
            std::string detail = v.kind;
            if (!v.type.empty())
                detail += (detail.empty() ? "" : " ") + v.type;
            if (!detail.empty())
                item.detail = optional<std::string>(std::move(detail));
            items.push_back(std::move(item));
        }

        for (const auto& m : index.modules) {
            if (tok.cancelled) throw CompletionCancelled{};
            const bool is_iface = index.interface_names.count(m.name) > 0;
            const bool is_pkg   = index.package_names.count(m.name)   > 0;
            const lsCompletionItemKind k = is_iface ? lsCompletionItemKind::Interface
                                                     : lsCompletionItemKind::Module;
            auto it = make_item(m.name, k);
            it.detail = optional<std::string>(is_iface ? std::string("interface")
                                              : is_pkg  ? std::string("package")
                                                        : std::string("module"));
            items.push_back(std::move(it));

            for (const auto& p : m.ports) {
                auto pi = make_item(p.name, lsCompletionItemKind::Variable);
                if (!p.direction.empty() || !p.type.empty())
                    pi.detail = optional<std::string>(p.direction + " " + p.type);
                items.push_back(std::move(pi));
            }
        }

        for (const auto& c : index.classes) {
            if (tok.cancelled) throw CompletionCancelled{};
            items.push_back(make_item(c.name, lsCompletionItemKind::Class));
        }

        for (const auto& t : index.typedefs) {
            if (tok.cancelled) throw CompletionCancelled{};
            if (t.is_enum) {
                auto it = make_item(t.name, lsCompletionItemKind::Enum);
                it.detail = optional<std::string>("enum");
                items.push_back(std::move(it));
                for (const auto& em : t.enum_members)
                    items.push_back(make_item(em.name, lsCompletionItemKind::EnumMember));
            } else {
                auto it = make_item(t.name, lsCompletionItemKind::TypeParameter);
                if (!t.resolved.empty()) it.detail = optional<std::string>(t.resolved);
                items.push_back(std::move(it));
            }
        }

        for (const auto& mac : index.macros) {
            if (tok.cancelled) throw CompletionCancelled{};
            items.push_back(make_item("`" + mac.name,
                                       mac.is_function_like ? lsCompletionItemKind::Function
                                                            : lsCompletionItemKind::Constant));
        }

        return items;
    }
};

// MemberProvider: fields/methods for class, ports for module/interface.
class MemberProvider : public CompletionProvider {
  public:
    bool accepts(const CompletionContext& ctx) const override {
        return ctx.kind == CompletionContextKind::MemberAccess && !ctx.scope_name.empty();
    }

    std::vector<lsCompletionItem> provide(const CompletionContext& ctx,
                                           const SyntaxIndex& index,
                                           const CancellationToken& /*tok*/) const override {
        std::vector<lsCompletionItem> items;
        std::string name = ctx.scope_name;

        // Resolve "variable." through the visible variable's declared type.
        // Without this, member access only works for "ClassName." or
        // "ModuleName.", which is rarely what users type in real code.
        if (auto value_type = type_of_value(index, ctx.current_scope_name, name)) {
            const std::string base = completion_base_type_name(*value_type);
            if (!base.empty())
                name = base;
        }

        // Class → fields + methods
        if (const auto it = index.class_by_name.find(name);
            it != index.class_by_name.end()) {
            const auto& cls = index.classes[it->second];
            for (const auto& f : cls.fields) {
                auto item = make_item(f.name, lsCompletionItemKind::Field);
                if (!f.type.empty()) item.detail = optional<std::string>(f.type);
                items.push_back(std::move(item));
            }
            for (const auto& m : cls.methods) {
                auto item = make_item(m.name, lsCompletionItemKind::Method);
                if (!m.return_type.empty())
                    item.detail = optional<std::string>(m.return_type);
                items.push_back(std::move(item));
            }
            return items;
        }

        // Module or interface → ports / signals
        if (const auto it = index.module_by_name.find(name);
            it != index.module_by_name.end()) {
            const auto& mod = index.modules[it->second];
            for (const auto& p : mod.ports) {
                auto item = make_item(p.name, lsCompletionItemKind::Field);
                if (!p.direction.empty() || !p.type.empty())
                    item.detail = optional<std::string>(p.direction + " " + p.type);
                items.push_back(std::move(item));
            }
            return items;
        }

        // Typedef resolution:
        //   * typedef struct packed { ... } packet_t;  -> packet_var.<fields>
        //   * typedef some_class alias_t;              -> alias_var.<class members>
        if (const auto it = index.typedef_by_name.find(name);
            it != index.typedef_by_name.end()) {
            const auto& td = index.typedefs[it->second];
            if (td.is_struct) {
                for (const auto& f : td.fields) {
                    auto item = make_item(f.name, lsCompletionItemKind::Field);
                    if (!f.type.empty())
                        item.detail = optional<std::string>(f.type);
                    items.push_back(std::move(item));
                }
                return items;
            }
            if (!td.resolved.empty() && td.resolved != name) {
                CompletionContext inner = ctx;
                inner.scope_name = td.resolved;
                return provide(inner, index, CancellationToken{});
            }
        }

        return items; // unresolved — return empty, don't pollute
    }
};

// NamedPortProvider: .portname( ) connections inside module instantiations.
class NamedPortProvider : public CompletionProvider {
  public:
    bool accepts(const CompletionContext& ctx) const override {
        return ctx.kind == CompletionContextKind::NamedPort;
    }

    std::vector<lsCompletionItem> provide(const CompletionContext& ctx,
                                           const SyntaxIndex& index,
                                           const CancellationToken& /*tok*/) const override {
        if (ctx.scope_name.empty()) return {};
        const auto it = index.module_by_name.find(ctx.scope_name);
        if (it == index.module_by_name.end()) return {};

        const auto& mod = index.modules[it->second];
        std::vector<lsCompletionItem> items;
        items.reserve(mod.ports.size());

        auto make_port_item = [&](const PortEntry& p) {
            lsCompletionItem item;
            item.label = "." + p.name;
            item.filterText = optional<std::string>(p.name);
            item.kind = optional<lsCompletionItemKind>(lsCompletionItemKind::Field);
            item.detail = optional<std::string>(p.direction + " " + p.type);
            // The label keeps the SystemVerilog spelling (".port") for the
            // completion menu, but insertText intentionally omits the leading
            // dot.  The dot is already present in the editor buffer because it
            // is the trigger character.  Supplying only "port(...)" lets
            // clients use their normal word replacement for typed prefixes:
            //
            //     .|   + insert "i_clk(...)" -> .i_clk(...)
            //     .i|  + replace "i"        -> .i_clk(...)
            //
            // Keep this as a snippet so accepting the completion selects the
            // connected signal placeholder.  The Neovim plugin side sets
            // buffer-local 'completeopt' without "popup"/"preview" so moving
            // through the menu does not ask Neovim to Tree-sitter-highlight a
            // synthesized snippet preview.
            const std::string snippet = p.name + "(${1:" + p.name + "})";
            item.insertText = optional<std::string>(snippet);
            item.insertTextFormat =
                optional<lsInsertTextFormat>(lsInsertTextFormat::Snippet);
            return item;
        };

        for (const auto& p : mod.ports) {
            // Parameter declarations are stored in ModuleEntry::ports for
            // historical index compatibility.  Named-port completion must only
            // offer real ports; otherwise typing `.` in an instantiation mixes
            // `.WIDTH(...)` with `.clk(...)`.
            if (p.direction == "parameter" || p.direction == "localparam")
                continue;
            if (!ctx.connected_ports.count(p.name))
                items.push_back(make_port_item(p));
        }

        return items;
    }
};

// ParameterProvider: .param( ) overrides inside #( ).
class ParameterProvider : public CompletionProvider {
  public:
    bool accepts(const CompletionContext& ctx) const override {
        return ctx.kind == CompletionContextKind::Parameter;
    }

    std::vector<lsCompletionItem> provide(const CompletionContext& ctx,
                                           const SyntaxIndex& index,
                                           const CancellationToken& /*tok*/) const override {
        if (ctx.scope_name.empty()) return {};
        const auto it = index.module_by_name.find(ctx.scope_name);
        if (it == index.module_by_name.end()) return {};

        const auto& mod = index.modules[it->second];
        std::vector<lsCompletionItem> items;
        for (const auto& p : mod.ports) {
            // Parameter completion is the mirror image of named-port
            // completion: only parameter/localparam declarations from #( ... )
            // should be suggested here, never ordinary I/O ports.
            if (p.direction != "parameter" && p.direction != "localparam")
                continue;
            if (ctx.connected_ports.count(p.name)) continue;
            lsCompletionItem item;
            item.label = "." + p.name;
            item.filterText = optional<std::string>(p.name);
            item.kind = optional<lsCompletionItemKind>(lsCompletionItemKind::Constant);
            {
                std::string detail = p.type;
                if (!p.default_value.empty())
                    detail += (detail.empty() ? "" : " ") + std::string("= ") + p.default_value;
                if (!detail.empty())
                    item.detail = optional<std::string>(std::move(detail));
            }
            // See NamedPortProvider above: the trigger '.' already exists in
            // the buffer, so insert only the parameter call snippet.
            const std::string snippet = p.name + "(${1:})";
            item.insertText = optional<std::string>(snippet);
            item.insertTextFormat =
                optional<lsInsertTextFormat>(lsInsertTextFormat::Snippet);
            items.push_back(std::move(item));
        }
        return items;
    }
};

// PackageScopeProvider: symbols visible via pkg:: and static class scope.
class PackageScopeProvider : public CompletionProvider {
  public:
    bool accepts(const CompletionContext& ctx) const override {
        return ctx.kind == CompletionContextKind::PackageScope && !ctx.scope_name.empty();
    }

    std::vector<lsCompletionItem> provide(const CompletionContext& ctx,
                                           const SyntaxIndex& index,
                                           const CancellationToken& /*tok*/) const override {
        std::vector<lsCompletionItem> items;
        std::unordered_set<std::string> seen;

        auto add_item = [&](std::string_view label, lsCompletionItemKind kind) {
            if (label.empty())
                return;
            std::string key(label);
            if (!seen.insert(key).second)
                return;
            items.push_back(make_item(key, kind));
        };

        // Package scope: uvm_pkg::, my_pkg::, ...
        //
        // Package symbols are collected while building SyntaxIndex.  Their
        // detailed kind can be recovered from the global class/typedef/value
        // tables, but returning Value is still safe when the detailed table
        // does not have a match (for example for declarations represented
        // differently by slang).
        if (const auto it = index.package_symbols.find(ctx.scope_name);
            it != index.package_symbols.end()) {
            for (const auto& sym : it->second) {
                lsCompletionItemKind kind = lsCompletionItemKind::Value;
                if (index.class_by_name.count(sym))
                    kind = lsCompletionItemKind::Class;
                else if (index.typedef_by_name.count(sym))
                    kind = lsCompletionItemKind::TypeParameter;
                else {
                    for (const auto& v : index.values) {
                        if (v.name == sym && v.parent_scope == ctx.scope_name) {
                            kind = (v.kind == "function") ? lsCompletionItemKind::Function
                                                           : lsCompletionItemKind::Variable;
                            break;
                        }
                    }
                }
                add_item(sym, kind);
            }
        }

        // Recovery for include-heavy packages.
        //
        // Some libraries, notably UVM, build a package by `include`-ing many
        // class files.  Depending on how far the parser got through an
        // incomplete editing snapshot, the package declaration can be known
        // while the package-symbol list is temporarily sparse.  Returning no
        // completion after `uvm_pkg::` is worse than a slightly broad recovery
        // list, so when the scope name is a known package we fall back to the
        // indexed global symbols.  Symbols that explicitly record
        // parent_scope == package still remain package-filtered; classes and
        // typedefs currently do not store parent_scope, so they are included
        // as best-effort package-visible candidates.
        if (index.package_names.count(ctx.scope_name)) {
            for (const auto& c : index.classes)
                add_item(c.name, lsCompletionItemKind::Class);
            for (const auto& t : index.typedefs)
                add_item(t.name, t.is_enum ? lsCompletionItemKind::Enum
                                            : lsCompletionItemKind::TypeParameter);
            for (const auto& v : index.values) {
                if (v.parent_scope != ctx.scope_name)
                    continue;
                const auto kind = (v.kind == "function") ? lsCompletionItemKind::Function
                                                          : lsCompletionItemKind::Variable;
                add_item(v.name, kind);
            }
            if (!items.empty())
                return items;
        }

        // Static class scope: uvm_config_db#(uvm_object)::, my_class::, ...
        //
        // Completion context detection normalizes parameterized class names to
        // their base declaration name, so `uvm_config_db#(T)::` and
        // `uvm_config_db::` both look up ClassEntry("uvm_config_db").  We offer
        // methods first because the main use case is static API calls such as
        // get/set; fields are also included for completeness and for ordinary
        // user-defined classes.
        if (const auto cit = index.class_by_name.find(ctx.scope_name);
            cit != index.class_by_name.end()) {
            const auto& cls = index.classes[cit->second];
            for (const auto& method : cls.methods) {
                auto item = make_item(method.name, method.is_task ? lsCompletionItemKind::Method
                                                                  : lsCompletionItemKind::Function);
                if (!method.return_type.empty())
                    item.detail = optional<std::string>(method.return_type);
                items.push_back(std::move(item));
            }
            for (const auto& field : cls.fields) {
                auto item = make_item(field.name, lsCompletionItemKind::Field);
                if (!field.type.empty())
                    item.detail = optional<std::string>(field.type);
                items.push_back(std::move(item));
            }
        }
        return items;
    }
};

// MacroProvider: `define macro names (Macro trigger context).
class MacroProvider : public CompletionProvider {
  public:
    bool accepts(const CompletionContext& ctx) const override {
        return ctx.kind == CompletionContextKind::Macro;
    }

    std::vector<lsCompletionItem> provide(const CompletionContext& /*ctx*/,
                                           const SyntaxIndex& index,
                                           const CancellationToken& /*tok*/) const override {
        std::vector<lsCompletionItem> items;
        for (const auto& mac : index.macros) {
            auto item = make_item(mac.name,
                                   mac.is_function_like ? lsCompletionItemKind::Function
                                                        : lsCompletionItemKind::Constant);
            if (mac.is_function_like && !mac.params.empty()) {
                std::string snip = mac.name + "(";
                for (size_t i = 0; i < mac.params.size(); ++i) {
                    if (i) snip += ", ";
                    char buf[128];
                    std::snprintf(buf, sizeof(buf), "${%zu:%s}", i + 1,
                                  mac.params[i].c_str());
                    snip += buf;
                }
                snip += ")";
                item.insertText = optional<std::string>(std::move(snip));
                item.insertTextFormat =
                    optional<lsInsertTextFormat>(lsInsertTextFormat::Snippet);
            }
            items.push_back(std::move(item));
        }
        return items;
    }
};

class NewExpressionProvider : public CompletionProvider {
  public:
    bool accepts(const CompletionContext& ctx) const override {
        return ctx.kind == CompletionContextKind::NewExpression;
    }

    std::vector<lsCompletionItem> provide(const CompletionContext& /*ctx*/,
                                           const SyntaxIndex& index,
                                           const CancellationToken& tok) const override {
        std::vector<lsCompletionItem> items;
        for (const auto& c : index.classes) {
            if (tok.cancelled) throw CompletionCancelled{};
            auto item = make_item(c.name, lsCompletionItemKind::Class);
            item.detail = optional<std::string>("class constructor");
            item.insertText = optional<std::string>(c.name + "::new($0)");
            item.insertTextFormat = optional<lsInsertTextFormat>(lsInsertTextFormat::Snippet);
            items.push_back(std::move(item));
        }
        return items;
    }
};

class EventControlProvider : public CompletionProvider {
  public:
    bool accepts(const CompletionContext& ctx) const override {
        return ctx.kind == CompletionContextKind::EventControl;
    }

    std::vector<lsCompletionItem> provide(const CompletionContext& ctx,
                                           const SyntaxIndex& index,
                                           const CancellationToken& tok) const override {
        std::vector<lsCompletionItem> items;
        for (const auto& v : index.values) {
            if (tok.cancelled) throw CompletionCancelled{};
            if (!ctx.current_scope_name.empty() && !v.parent_scope.empty() &&
                v.parent_scope != ctx.current_scope_name &&
                !index.package_names.count(v.parent_scope))
                continue;
            auto item = make_item(v.name, lsCompletionItemKind::Variable);
            std::string detail = v.kind;
            if (!v.type.empty())
                detail += (detail.empty() ? "" : " ") + v.type;
            if (!detail.empty())
                item.detail = optional<std::string>(std::move(detail));
            items.push_back(std::move(item));
        }
        return items;
    }
};

// SnippetProvider: structural snippets for common SV constructs.
class SnippetProvider : public CompletionProvider {
  public:
    bool accepts(const CompletionContext& ctx) const override {
        return ctx.kind == CompletionContextKind::Identifier ||
               ctx.kind == CompletionContextKind::Unknown;
    }

    std::vector<lsCompletionItem> provide(const CompletionContext& /*ctx*/,
                                           const SyntaxIndex& /*index*/,
                                           const CancellationToken& /*tok*/) const override {
        struct S {
            const char* label;
            const char* body;
        };
        // clang-format off
        static const S kSnips[] = {
            {"module",       "module ${1:name} (\n    $0\n);\n\nendmodule"},
            {"interface",    "interface ${1:name} (\n    $0\n);\n\nendinterface"},
            {"package",      "package ${1:name};\n    $0\nendpackage"},
            {"class",        "class ${1:name};\n    $0\nendclass"},
            {"always_ff",    "always_ff @(posedge ${1:clk} or negedge ${2:rst_n}) begin\n    $0\nend"},
            {"always_comb",  "always_comb begin\n    $0\nend"},
            {"always_latch", "always_latch begin\n    $0\nend"},
            {"function",     "function ${1:void} ${2:name}($3);\n    $0\nendfunction"},
            {"task",         "task ${1:name}($2);\n    $0\nendtask"},
            {"covergroup",   "covergroup ${1:name};\n    $0\nendgroup"},
            {"generate",     "generate\n    $0\nendgenerate"},
            {"case",         "case (${1:expr})\n    default: $0\nendcase"},
            {"for",          "for (int ${1:i} = 0; ${1:i} < ${2:N}; ++${1:i}) begin\n    $0\nend"},
            {"foreach",      "foreach (${1:arr}[${2:i}]) begin\n    $0\nend"},
            {nullptr, nullptr}
        };
        // clang-format on
        std::vector<lsCompletionItem> items;
        for (int i = 0; kSnips[i].label; ++i) {
            lsCompletionItem item;
            item.label = kSnips[i].label;
            item.kind = optional<lsCompletionItemKind>(lsCompletionItemKind::Snippet);
            item.detail = optional<std::string>("snippet");
            item.insertText = optional<std::string>(kSnips[i].body);
            item.insertTextFormat =
                optional<lsInsertTextFormat>(lsInsertTextFormat::Snippet);
            items.push_back(std::move(item));
        }
        return items;
    }
};

// FileProvider: .svh/.vh header files listed in the design filelist.
class FileProvider : public CompletionProvider {
  public:
    bool accepts(const CompletionContext& ctx) const override {
        return ctx.kind == CompletionContextKind::IncludeFile;
    }

    std::vector<lsCompletionItem> provide(const CompletionContext& ctx,
                                           const SyntaxIndex& /*index*/,
                                           const CancellationToken& tok) const override {
        std::vector<lsCompletionItem> items;
        for (const auto& path : ctx.header_files) {
            if (tok.cancelled) throw CompletionCancelled{};
            const std::string fname =
                std::filesystem::path(path).filename().string();
            lsCompletionItem item;
            item.label = fname;
            item.kind = optional<lsCompletionItemKind>(lsCompletionItemKind::File);
            item.insertText = optional<std::string>(fname + "\"");
            item.insertTextFormat =
                optional<lsInsertTextFormat>(lsInsertTextFormat::PlainText);
            items.push_back(std::move(item));
        }
        return items;
    }
};

// ── CompletionEngine ──────────────────────────────────────────────────────────

CompletionEngine::CompletionEngine() {
    // Semantic providers first (they have narrower accepts() and higher
    // relevance); structural providers last.
    providers_.push_back(std::make_unique<MemberProvider>());
    providers_.push_back(std::make_unique<NamedPortProvider>());
    providers_.push_back(std::make_unique<ParameterProvider>());
    providers_.push_back(std::make_unique<PackageScopeProvider>());
    providers_.push_back(std::make_unique<MacroProvider>());
    providers_.push_back(std::make_unique<FileProvider>());
    providers_.push_back(std::make_unique<NewExpressionProvider>());
    providers_.push_back(std::make_unique<EventControlProvider>());
    providers_.push_back(std::make_unique<IdentifierProvider>());
    providers_.push_back(std::make_unique<SnippetProvider>());
    providers_.push_back(std::make_unique<KeywordProvider>());
}

CompletionContext CompletionEngine::detect_context(const DocumentState& state, int line, int col,
                                                    const SyntaxIndex& index) const {
    const std::string& text = state.text;
    CompletionContext ctx;
    ctx.line = line;
    ctx.col = col;

    const size_t offset = position_to_offset(text, line, col);
    ctx.current_scope_name = infer_current_scope(index, line);
    ctx.keyword_context = infer_keyword_context(state, offset);

    // Step 1: read the prefix (identifier chars already typed after trigger)
    size_t pos = offset;
    ctx.prefix = backward_read_word(text, pos);
    ctx.expected_type = infer_assignment_lhs_type(text, pos, index, ctx.current_scope_name);
    // pos now at start of prefix

    if (pos == 0) {
        ctx.kind = CompletionContextKind::Identifier;
        return ctx;
    }

    // Step 2: examine the character(s) just before the prefix
    const char c1 = text[pos - 1];

    // --- foo.bar | or foo.| -----------------------------------------------
    if (c1 == '.') {
        const size_t dot_offset = pos - 1; // byte index of '.'
        --pos;                             // consume '.'
        backward_skip_ws(text, pos);
        std::string scope = backward_read_word(text, pos);

        if (!scope.empty()) {
            // identifier.prefix → MemberAccess
            ctx.kind = CompletionContextKind::MemberAccess;
            ctx.scope_name = std::move(scope);
        } else {
            // bare '.' (no preceding identifier) → NamedPort or Parameter
            const size_t open = find_opening_paren(text, dot_offset);
            if (open < text.size() && open > 0 && text[open - 1] == '#') {
                ctx.kind = CompletionContextKind::Parameter;
                // Module name sits directly before '#'; no instance name between
                size_t hp = open - 1; // '#'
                backward_skip_ws(text, hp);
                ctx.scope_name = backward_read_word(text, hp);
            } else {
                ctx.kind = CompletionContextKind::NamedPort;
                ctx.scope_name =
                    find_enclosing_instance(text, dot_offset).value_or(std::string{});
            }

            // Collect already-connected port/param names from .portname( patterns
            // between the opening paren and the current dot.
            if (open < dot_offset) {
                size_t scan = open + 1;
                while (scan < dot_offset) {
                    if (text[scan] == '.') {
                        ++scan;
                        const size_t name_start = scan;
                        while (scan < dot_offset && is_ident_char(text[scan]))
                            ++scan;
                        if (scan > name_start) {
                            std::string port_name = text.substr(name_start, scan - name_start);
                            // confirm followed (after optional ws) by '('
                            size_t tmp = scan;
                            while (tmp < dot_offset &&
                                   std::isspace((unsigned char)text[tmp]))
                                ++tmp;
                            if (tmp < dot_offset && text[tmp] == '(')
                                ctx.connected_ports.insert(std::move(port_name));
                        }
                    } else {
                        ++scan;
                    }
                }
            }
        }
        return ctx;
    }

    // --- pkg::sym | / class#(...)::sym | ---------------------------------
    //
    // Scope completion is intentionally SyntaxTree-driven.  The raw text has
    // already been used only to peel off the typed completion prefix; from this
    // point on the `::` operator and its left-hand scope are found from slang
    // tokens / NameSyntax nodes.  If slang cannot parse a scope operator at
    // this edit snapshot, we do not fall back to a backward source scanner.
    if (state.tree) {
        ctx.scope_name =
            syntax_scope_base_before_double_colon(*state.tree, pos).value_or(std::string{});
        if (!ctx.scope_name.empty()) {
            ctx.kind = CompletionContextKind::PackageScope;
            return ctx;
        }
    }

    // --- `macro_name | ----------------------------------------------------
    if (c1 == '`') {
        ctx.kind = CompletionContextKind::Macro;
        return ctx;
    }

    // --- `include "path | -------------------------------------------------
    if (c1 == '"') {
        size_t tmp = pos - 1; // before '"'
        backward_skip_ws(text, tmp);
        const std::string kw = backward_read_word(text, tmp);
        if (kw == "include" && tmp > 0 && text[tmp - 1] == '`') {
            ctx.kind = CompletionContextKind::IncludeFile;
            return ctx;
        }
        ctx.kind = CompletionContextKind::Identifier;
        return ctx;
    }

    // --- class construction after "new " ---------------------------------
    {
        size_t tmp = pos;
        backward_skip_ws(text, tmp);
        const std::string kw = backward_read_word(text, tmp);
        if (kw == "new") {
            ctx.kind = CompletionContextKind::NewExpression;
            return ctx;
        }
    }

    // --- event control @( ... | ) -----------------------------------------
    {
        const size_t open = find_opening_paren(text, pos);
        if (open < text.size() && paren_is_event_control(text, open)) {
            ctx.kind = CompletionContextKind::EventControl;
            return ctx;
        }
    }

    ctx.kind = CompletionContextKind::Identifier;
    return ctx;
}

void CompletionEngine::rank_and_sort(std::vector<lsCompletionItem>& items,
                                      const CompletionContext& ctx,
                                      const std::unordered_set<std::string>& local_names,
                                      const std::unordered_set<std::string>& expected_names,
                                      const std::unordered_set<std::string>& same_type_names) const {
    struct Scored {
        lsCompletionItem item;
        int score;
    };

    std::vector<Scored> scored;
    scored.reserve(items.size());

    for (auto& item : items) {
        const std::string_view candidate =
            item.filterText ? std::string_view(*item.filterText)
                            : std::string_view(item.label);
        const int s = prefix_score(candidate, ctx.prefix);
        if (s > 0 || ctx.prefix.empty()) {
            // Scope score: +30 if declared in the current document's index.
            const int scope = local_names.count(item.label) ? 30 : 0;
            const int expected = expected_names.count(item.label) ? 1000 : 0;
            const int same_type = same_type_names.count(item.label) ? 500 : 0;
            scored.push_back({std::move(item), s + scope + expected + same_type});
        }
    }

    std::stable_sort(scored.begin(), scored.end(), [](const Scored& a, const Scored& b) {
        if (a.score != b.score) return a.score > b.score;
        return a.item.label < b.item.label;
    });

    items.clear();
    items.reserve(scored.size());
    char buf[8];
    for (size_t i = 0; i < scored.size(); ++i) {
        std::snprintf(buf, sizeof(buf), "%05zu", i);
        scored[i].item.sortText = optional<std::string>(std::string(buf));
        items.push_back(std::move(scored[i].item));
    }
}

CompletionList CompletionEngine::complete(const lsTextDocumentPositionParams& params,
                                           const DocumentState& state,
                                           const Analyzer& analyzer,
                                           const CancellationToken& tok) const {
    CompletionList result;
    result.isIncomplete = false;

    // Build a merged index: current document + extra files from .f list
    SyntaxIndex merged = state.index;
    analyzer.merge_extra_file_modules(merged);

    // Collect symbol names declared in the current document for scope scoring.
    std::unordered_set<std::string> local_names;
    for (const auto& m : state.index.modules)  local_names.insert(m.name);
    for (const auto& c : state.index.classes)  local_names.insert(c.name);
    for (const auto& t : state.index.typedefs) local_names.insert(t.name);
    for (const auto& mac : state.index.macros) local_names.insert(mac.name);
    for (const auto& v : state.index.values)   local_names.insert(v.name);

    const int line = params.position.line;
    const int col  = params.position.character;

    CompletionContext ctx;
    try {
        ctx = detect_context(state, line, col, merged);
    } catch (...) {
        ctx.kind = CompletionContextKind::Identifier;
    }

    if (ctx.kind == CompletionContextKind::IncludeFile) {
        for (const auto& p : analyzer.extra_files()) {
            const std::string ext = std::filesystem::path(p).extension().string();
            if (ext == ".svh" || ext == ".vh")
                ctx.header_files.push_back(p);
        }
    }

    std::vector<lsCompletionItem> all_items;

    try {
        for (const auto& provider : providers_) {
            if (tok.cancelled) throw CompletionCancelled{};
            if (!provider->accepts(ctx)) continue;
            auto items = provider->provide(ctx, merged, tok);
            all_items.insert(all_items.end(),
                             std::make_move_iterator(items.begin()),
                             std::make_move_iterator(items.end()));
        }
    } catch (const CompletionCancelled&) {
        return result; // return empty on cancellation
    }

    // Degradation: never return empty for Identifier/Unknown context
    if (all_items.empty() && ctx.kind != CompletionContextKind::MemberAccess &&
        ctx.kind != CompletionContextKind::PackageScope &&
        ctx.kind != CompletionContextKind::NamedPort &&
        ctx.kind != CompletionContextKind::Parameter) {
        KeywordProvider fallback;
        CancellationToken dummy;
        all_items = fallback.provide(ctx, merged, dummy);
    }

    const auto expected_names = enum_members_for_type(merged, ctx.expected_type);
    const auto same_type_names = value_names_for_type(merged, ctx);
    rank_and_sort(all_items, ctx, local_names, expected_names, same_type_names);
    result.items = std::move(all_items);
    return result;
}
