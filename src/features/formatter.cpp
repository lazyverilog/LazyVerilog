#include "formatter.hpp"
#include <algorithm>
#include <cctype>
#include <slang/parsing/Token.h>
#include <slang/syntax/AllSyntax.h>
#include <slang/syntax/SyntaxTree.h>
#include <slang/syntax/SyntaxVisitor.h>
#include <slang/text/SourceManager.h>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace slang;
using namespace slang::syntax;

namespace {

static int snap_to_indent_grid(int value, int indent_size) {
    if (indent_size <= 0)
        return value;
    return ((value + indent_size - 1) / indent_size) * indent_size;
}

static int tab_aligned_width(int value, const FormatOptions& opts) {
    return opts.tab_align ? snap_to_indent_grid(value, opts.indent_size) : value;
}

// ---------------------------------------------------------------------------
// Slang-based line classification
// ---------------------------------------------------------------------------

enum class LineKind { Other, PortDecl, VarDecl, Instantiation, ModuleHeader };

struct LineClassifier : SyntaxVisitor<LineClassifier> {
    const SourceManager& sm;
    std::unordered_map<int, LineKind> kinds; // 0-based line -> kind

    explicit LineClassifier(const SourceManager& s) : sm(s) {}

    int line_of(const SyntaxNode& node) const {
        auto tok = node.getFirstToken();
        if (!tok || !tok.location().valid())
            return -1;
        return (int)sm.getLineNumber(tok.location()) - 1; // convert to 0-based
    }

    void set(const SyntaxNode& node, LineKind k) {
        int ln = line_of(node);
        if (ln >= 0)
            kinds.emplace(ln, k);
    }

    void handle(const ImplicitAnsiPortSyntax& n) {
        set(n, LineKind::PortDecl);
        visitDefault(n);
    }
    void handle(const PortDeclarationSyntax& n) {
        set(n, LineKind::PortDecl);
        visitDefault(n);
    }
    void handle(const DataDeclarationSyntax& n) {
        set(n, LineKind::VarDecl);
        visitDefault(n);
    }
    void handle(const NetDeclarationSyntax& n) {
        set(n, LineKind::VarDecl);
        visitDefault(n);
    }
    void handle(const HierarchyInstantiationSyntax& n) {
        set(n, LineKind::Instantiation);
        visitDefault(n);
    }
    void handle(const ModuleDeclarationSyntax& n) {
        if (n.kind == SyntaxKind::ModuleDeclaration)
            set(n, LineKind::ModuleHeader);
        visitDefault(n);
    }
};

// ---------------------------------------------------------------------------
// FormattedLine — intermediate representation passed between formatter passes
// ---------------------------------------------------------------------------
//
// Each line carries its text content plus metadata (syntactic kind, disabled
// status).  Passes receive and return vectors of FormattedLine instead of flat
// strings, eliminating redundant split/join per pass and fragile line-number
// remapping for kind classification.

struct FormattedLine {
    std::string text;
    LineKind kind{LineKind::Other};
    bool disabled{false};
};

static std::string render_lines(const std::vector<FormattedLine>& lines) {
    std::string r;
    for (size_t k = 0; k < lines.size(); ++k) {
        if (k)
            r += '\n';
        r += lines[k].text;
    }
    return r;
}

static std::vector<FormattedLine> text_to_lines(const std::string& text,
                                                 LineKind kind = LineKind::Other) {
    std::vector<FormattedLine> lines;
    std::istringstream ss(text);
    std::string l;
    while (std::getline(ss, l))
        lines.push_back({std::move(l), kind, false});
    return lines;
}

// ---------------------------------------------------------------------------
// CST formatting token helpers
// ---------------------------------------------------------------------------

using slang::parsing::TokenKind;

struct Tok {
    TokenKind kind{TokenKind::Unknown};
    std::string text;
    std::string lo;
    int pos{0};
    bool whitespace{false};
    bool comment{false};
    bool directive{false};
};

enum class SD { MustAppend, MustWrap, Preserve, Undecided };

// ---------------------------------------------------------------------------
// Keyword sets
// ---------------------------------------------------------------------------

static const std::unordered_set<std::string> SV_KW = {
    "module",      "macromodule", "endmodule",    "interface",    "endinterface",  "program",
    "endprogram",  "package",     "endpackage",   "class",        "endclass",      "function",
    "endfunction", "task",        "endtask",      "begin",        "end",           "fork",
    "join",        "join_any",    "join_none",    "case",         "casex",         "casez",
    "caseinside",  "endcase",     "generate",     "endgenerate",  "covergroup",    "endgroup",
    "property",    "endproperty", "sequence",     "endsequence",  "checker",       "endchecker",
    "clocking",    "endclocking", "config",       "endconfig",    "primitive",     "endprimitive",
    "specify",     "endspecify",  "table",        "endtable",     "input",         "output",
    "inout",       "ref",         "logic",        "wire",         "reg",           "bit",
    "byte",        "shortint",    "int",          "longint",      "integer",       "real",
    "realtime",    "shortreal",   "time",         "string",       "chandle",       "event",
    "always",      "always_comb", "always_ff",    "always_latch", "initial",       "final",
    "assign",      "if",          "else",         "for",          "foreach",       "while",
    "do",          "repeat",      "forever",      "return",       "break",         "continue",
    "typedef",     "struct",      "union",        "enum",         "packed",        "unpacked",
    "parameter",   "localparam",  "defparam",     "virtual",      "static",        "automatic",
    "const",       "var",         "default",      "void",         "type",          "signed",
    "unsigned",    "modport",     "genvar",       "import",       "export",        "extern",
    "protected",   "local",       "posedge",      "negedge",      "edge",          "or",
    "and",         "not",         "assert",       "assume",       "cover",         "restrict",
    "unique",      "unique0",     "priority",     "inside",       "dist",          "rand",
    "randc",       "constraint",  "super",        "this",         "null",          "new",
    "expect",      "wait",        "wait_order",   "disable",      "force",         "release",
    "deassign",    "pullup",      "pulldown",     "supply0",      "supply1",       "tri",
    "tri0",        "tri1",        "triand",       "trior",        "trireg",        "wand",
    "wor",         "uwire",       "with",         "bind",         "let",           "cross",
    "bins",        "binsof",      "extends",      "implements",   "throughout",    "within",
    "iff",         "intersect",   "first_match",  "matches",      "tagged",        "wildcard",
    "solve",       "before",      "pure",         "context",      "timeprecision", "timeunit",
    "forkjoin",    "randcase",    "randsequence", "randomize",    "coverpoint",    "strong",
    "weak",
};

static const std::unordered_set<std::string> TYPE_KW = {
    "logic",   "wire",    "reg",  "bit",      "byte",      "shortint", "int",
    "longint", "integer", "real", "realtime", "shortreal", "time",     "string",
    "chandle", "event",   "void", "signed",   "unsigned",  "packed",
};

static const std::unordered_set<std::string> INDENT_OPEN = {
    "module",   "macromodule", "interface", "program",    "package",  "class",
    "function", "task",        "begin",     "fork",       "case",     "casex",
    "casez",    "caseinside",  "generate",  "covergroup", "property", "sequence",
    "checker",  "clocking",    "config",    "primitive",  "specify",
};

static const std::unordered_set<std::string> BLOCK_OPEN = {
    "begin",    "fork",     "case",    "casex",    "casez",  "caseinside", "generate", "covergroup",
    "property", "sequence", "checker", "clocking", "config", "primitive",  "specify",
};

static const std::unordered_set<std::string> INDENT_CLOSE = {
    "endmodule",   "endinterface", "endprogram",  "endpackage",  "endclass",   "endfunction",
    "endtask",     "end",          "join",        "join_any",    "join_none",  "endcase",
    "endgenerate", "endgroup",     "endproperty", "endsequence", "endchecker", "endclocking",
    "endconfig",   "endprimitive", "endspecify",  "endtable",
};

static const std::unordered_set<std::string> ALWAYS_UNARY = {
    "~", "!", "~&", "~|", "~^", "^~", "++", "--",
};

static const std::unordered_set<std::string> ALWAYS_BINARY = {
    "===", "!==", "==", "!=", ">=", "->", "<->", "&&",  "||",   "**",   "##", "|->", "+=", "-=",
    "*=",  "/=",  "%=", "&=", "|=", "^=", "<<=", ">>=", "<<<=", ">>>=", "*",  "/",   "%",
};

static const std::unordered_set<std::string> PP_COND_WITH = {"`ifdef", "`ifndef", "`elsif"};
static const std::unordered_set<std::string> PP_COND_BARE = {"`else", "`endif"};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::string lower(const std::string& s) {
    std::string r = s;
    for (auto& c : r)
        c = (char)std::tolower((unsigned char)c);
    return r;
}

static bool has(const std::unordered_set<std::string>& s, const std::string& k) {
    return s.count(k) > 0;
}

static bool is_keyword(const Tok& t) {
    return !t.directive && !t.comment && !t.whitespace && has(SV_KW, t.lo);
}

static bool is_identifier(const Tok& t) {
    return !t.directive && !t.comment && !t.whitespace &&
           (t.kind == TokenKind::Identifier || t.kind == TokenKind::SystemIdentifier ||
            (!t.text.empty() &&
             (std::isalpha((unsigned char)t.text[0]) || t.text[0] == '_' || t.text[0] == '$' ||
              t.text[0] == '`') &&
             !is_keyword(t)));
}

static bool is_numeric(const Tok& t) {
    return t.kind == TokenKind::IntegerLiteral || t.kind == TokenKind::IntegerBase ||
           t.kind == TokenKind::UnbasedUnsizedLiteral || t.kind == TokenKind::RealLiteral ||
           t.kind == TokenKind::TimeLiteral;
}

static bool is_open_group(const Tok& t) { return t.text == "(" || t.text == "[" || t.text == "{"; }

static bool is_close_group(const Tok& t) { return t.text == ")" || t.text == "]" || t.text == "}"; }

static bool is_hierarchy(const Tok& t) { return t.text == "." || t.text == "::"; }

static bool is_unary_op(const Tok& t) { return has(ALWAYS_UNARY, t.text); }

static bool is_control_keyword(const Tok& t) {
    return is_keyword(t) &&
           (t.lo == "if" || t.lo == "for" || t.lo == "foreach" || t.lo == "while" ||
            t.lo == "repeat" || t.lo == "case" || t.lo == "casex" || t.lo == "casez");
}

static bool is_procedural_event_keyword(const Tok& t) {
    return is_keyword(t) && (t.lo == "always" || t.lo == "always_ff" || t.lo == "always_comb" ||
                             t.lo == "always_latch");
}

static bool is_binary_op(const Tok& t) {
    static const std::unordered_set<std::string> EXTRA_BINARY = {
        "+",   "-",   "<=", "<",   ">",   "?",      "<<", ">>",
        "<<<", ">>>", "->", "<->", "|->", "inside", "&&&"};
    return has(ALWAYS_BINARY, t.text) || has(EXTRA_BINARY, t.text);
}

static bool is_assignment_op(const Tok& t, bool in_parens = false) {
    if (t.text == "<=" && in_parens)
        return false;
    static const std::unordered_set<std::string> ASSIGN = {
        "=", "<=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>=", "<<<=", ">>>="};
    return has(ASSIGN, t.text);
}

static bool binary_space_before(const std::string& mode) {
    return mode == "before" || mode == "both";
}

static bool binary_space_after(const std::string& mode) {
    return mode == "after" || mode == "both";
}

enum class ParenKind { Ordinary, FunctionCall, EventControl };

static ParenKind classify_paren(const Tok& L, bool procedural_at = false) {
    if (L.text == "@")
        return procedural_at ? ParenKind::EventControl : ParenKind::FunctionCall;
    if (is_identifier(L) || L.text == "]")
        return ParenKind::FunctionCall;
    return ParenKind::Ordinary;
}

static bool looks_unary_context(const Tok& L, const Tok& R) {
    if (R.text != "+" && R.text != "-")
        return false;
    return L.text == "(" || L.text == "[" || L.text == "{" || L.text == "," || L.text == ";" ||
           L.text == ":" || L.text == "?" || L.text == "=" || is_binary_op(L) || is_keyword(L);
}

static bool is_indexed_part_select_op_pair(const Tok& A, const Tok& B) {
    return (A.text == "+" || A.text == "-") && B.text == ":";
}

static bool is_indexed_part_select_op(const Tok& t) { return t.text == "+:" || t.text == "-:"; }

// ---------------------------------------------------------------------------
// Disabled ranges (verilog_format: off/on and `define)
// ---------------------------------------------------------------------------

// Match "// <optional whitespace> verilog_format <optional whitespace> : <optional whitespace> <word>"
// case-insensitive.  Returns the position past the matched word, or std::string::npos.
static size_t match_fmt_comment(const std::string& src, size_t pos, const std::string& word) {
    size_t n = src.size();
    if (pos + 1 >= n || src[pos] != '/' || src[pos + 1] != '/')
        return std::string::npos;
    size_t i = pos + 2;
    while (i < n && (src[i] == ' ' || src[i] == '\t'))
        ++i;
    const char* tag = "verilog_format";
    for (size_t k = 0; tag[k]; ++k, ++i) {
        if (i >= n || std::tolower((unsigned char)src[i]) != tag[k])
            return std::string::npos;
    }
    while (i < n && (src[i] == ' ' || src[i] == '\t'))
        ++i;
    if (i >= n || src[i] != ':')
        return std::string::npos;
    ++i;
    while (i < n && (src[i] == ' ' || src[i] == '\t'))
        ++i;
    for (size_t k = 0; k < word.size(); ++k, ++i) {
        if (i >= n || std::tolower((unsigned char)src[i]) != (unsigned char)word[k])
            return std::string::npos;
    }
    // Word boundary: next char must not be alphanumeric/underscore
    if (i < n && (std::isalnum((unsigned char)src[i]) || src[i] == '_'))
        return std::string::npos;
    // Advance to end of line
    while (i < n && src[i] != '\n')
        ++i;
    return i;
}

static std::vector<std::pair<int, int>> find_disabled(const std::string& src) {
    std::vector<std::pair<int, int>> out;
    size_t n = src.size();

    // Find verilog_format: off/on regions
    for (size_t i = 0; i < n; ++i) {
        size_t off_end = match_fmt_comment(src, i, "off");
        if (off_end == std::string::npos)
            continue;
        int off_pos = (int)i;
        // Search for matching "on"
        int end = (int)n;
        for (size_t j = off_end; j < n; ++j) {
            size_t on_end = match_fmt_comment(src, j, "on");
            if (on_end != std::string::npos) {
                end = (int)j;
                break;
            }
        }
        out.push_back({off_pos, end});
        i = (size_t)end;
    }

    // Find `define blocks (including multi-line with \ continuation)
    for (size_t i = 0; i < n;) {
        // Must be at start of line (or start of file)
        if (i > 0 && src[i - 1] != '\n') {
            ++i;
            continue;
        }
        // Skip leading whitespace
        size_t ls = i;
        while (ls < n && (src[ls] == ' ' || src[ls] == '\t'))
            ++ls;
        if (ls + 7 > n || src.compare(ls, 7, "`define") != 0) {
            // Skip to next line
            while (i < n && src[i] != '\n')
                ++i;
            if (i < n)
                ++i;
            continue;
        }
        // Word boundary after `define
        if (ls + 7 < n && (std::isalnum((unsigned char)src[ls + 7]) || src[ls + 7] == '_')) {
            while (i < n && src[i] != '\n')
                ++i;
            if (i < n)
                ++i;
            continue;
        }
        // Found `define at ls.  Consume lines with trailing backslash continuation.
        size_t j = ls;
        while (true) {
            while (j < n && src[j] != '\n')
                ++j;
            // Check if line ends with backslash (before the newline)
            if (j > ls && src[j - 1] == '\\' && j < n) {
                ++j; // skip newline, continue to next line
            } else {
                break;
            }
        }
        out.push_back({(int)ls, (int)j});
        i = j;
        if (i < n)
            ++i;
    }

    std::sort(out.begin(), out.end());
    return out;
}

static bool in_disabled(int pos, const std::vector<std::pair<int, int>>& ranges) {
    for (auto& r : ranges)
        if (r.first <= pos && pos < r.second)
            return true;
    return false;
}

// ---------------------------------------------------------------------------
// Spacing rules — ported from SpacesRequiredBetween() in token-annotator.cc
// ---------------------------------------------------------------------------

static int spaces_req(const Tok& L, const Tok& R, const FormatOptions& opts, bool in_dim,
                      bool in_for_header, bool in_parens, ParenKind paren_kind,
                      bool procedural_at) {
    const auto& lx = L.text;
    const auto& ll = L.lo;
    const auto& rx = R.text;
    const auto& sp = opts.spacing;
    bool L_assign = is_assignment_op(L, in_parens);
    bool R_assign = is_assignment_op(R, in_parens);

    if (L.directive || R.directive)
        return 0;
    if (R.comment)
        return 1;
    if (lx == "(") {
        if (paren_kind == ParenKind::EventControl)
            return sp.space_inside_event_control_parens ? 1 : 0;
        if (paren_kind == ParenKind::Ordinary)
            return sp.space_inside_parens ? 1 : 0;
        return 0;
    }
    if (rx == ")") {
        if (paren_kind == ParenKind::EventControl)
            return sp.space_inside_event_control_parens ? 1 : 0;
        if (paren_kind == ParenKind::Ordinary)
            return sp.space_inside_parens ? 1 : 0;
        return 0;
    }
    if (lx == "[")
        return sp.space_inside_dimension_brackets ? 1 : 0;
    if (rx == "]")
        return sp.space_inside_dimension_brackets ? 1 : 0;
    if (is_open_group(L) || is_close_group(R))
        return 0;
    if (is_unary_op(L))
        return 0;
    if (is_hierarchy(L) && lx == "::")
        return 0;
    if (rx == ",")
        return 0;
    if (lx == ",")
        return 1;
    if (rx == ";")
        return in_for_header ? (binary_space_before(sp.semicolon_spacing) ? 1 : 0)
                             : ((lx == ":") ? 1 : 0);
    if (lx == ";")
        return in_for_header ? (binary_space_after(sp.semicolon_spacing) ? 1 : 0) : 1;
    if (lx == "@")
        return procedural_at ? (binary_space_after(sp.procedural_event_control_at_spacing) ? 1 : 0)
                             : 0;
    if (rx == "@")
        return procedural_at ? (binary_space_before(sp.procedural_event_control_at_spacing) ? 1 : 0)
                             : 1;
    if (is_unary_op(L) && rx == "{")
        return 0;
    if (L_assign || R_assign)
        return 1;
    if (in_dim && is_indexed_part_select_op(R))
        return binary_space_before(sp.indexed_part_select_spacing) ? 1 : 0;
    if (in_dim && is_indexed_part_select_op(L))
        return binary_space_after(sp.indexed_part_select_spacing) ? 1 : 0;
    if (in_dim && is_indexed_part_select_op_pair(L, R))
        return binary_space_before(sp.indexed_part_select_spacing) ? 1 : 0;
    if (in_dim && (lx == "+" || lx == "-") && rx == ":")
        return 0;
    if (in_dim && (lx == ":" || rx == ":"))
        return (lx == ":") ? (binary_space_after(sp.range_colon_spacing) ? 1 : 0)
                           : (binary_space_before(sp.range_colon_spacing) ? 1 : 0);
    if ((is_binary_op(L) && !L_assign) || (is_binary_op(R) && !R_assign)) {
        if (looks_unary_context(L, R))
            return 0;
        const std::string& mode =
            in_dim ? sp.dimension_binary_operator_spacing : sp.binary_operator_spacing;
        if (is_binary_op(R) && !R_assign)
            return binary_space_before(mode) ? 1 : 0;
        if (is_binary_op(L) && !L_assign)
            return binary_space_after(mode) ? 1 : 0;
    }
    if (is_binary_op(L) || is_binary_op(R)) {
        return 1;
    }
    if (is_hierarchy(L) || is_hierarchy(R))
        return 0;
    if (rx == "'" || lx == "'" || (!rx.empty() && rx[0] == '\'') || (!lx.empty() && lx[0] == '\''))
        return 0;
    if (rx == "(") {
        if (lx == "#")
            return 0;
        if (lx == ")")
            return 1;
        if (is_identifier(L))
            return 0;
        if (is_control_keyword(L))
            return sp.control_keyword_space ? 1 : 0;
        if (is_keyword(L))
            return 1;
        return 0;
    }
    if (lx == ":")
        return in_dim ? 0 : 1;
    if (rx == ":") {
        if (ll == "default")
            return 0;
        if (in_dim)
            return 0;
        if (is_identifier(L) || is_numeric(L) || is_close_group(L))
            return 0;
        return 1;
    }
    if (lx == "}")
        return 1;
    if (rx == "{")
        return is_keyword(L) ? 1 : 0;
    if (rx == "[") {
        if (lx == "]")
            return 0;
        if (is_keyword(L) && has(TYPE_KW, ll))
            return 1;
        return 0;
    }
    auto nm = [](const Tok& t) { return is_numeric(t) || is_identifier(t) || is_keyword(t); };
    if (nm(L) && nm(R))
        return 1;
    if (is_keyword(L))
        return 1;
    if (is_unary_op(L) || is_unary_op(R))
        return 0;
    if (lx == "#")
        return 0;
    if (rx == "#")
        return 1;
    if (is_keyword(R))
        return 1;
    if (lx == ")")
        return (rx == ":") ? 0 : 1;
    if (lx == "]")
        return 1;
    return 1;
}

// ---------------------------------------------------------------------------
// Break decisions — ported from BreakDecisionBetween()
// ---------------------------------------------------------------------------

static SD break_dec(const Tok& L, const Tok& R, const FormatOptions& opts, bool in_dim) {
    const auto& lx = L.text;
    const auto& ll = L.lo;
    const auto& rx = R.text;
    const auto& rl = R.lo;

    if (in_dim && lx != ":" && lx != "[" && lx != "]" && rx != ":" && rx != "[" && rx != "]")
        return SD::Preserve;
    if (R.directive || L.directive)
        return SD::MustWrap;
    if (L.comment && L.text.find('\n') != std::string::npos)
        return SD::MustWrap;
    if (L.comment && L.text.rfind("//", 0) == 0)
        return SD::MustWrap;
    if (is_unary_op(L))
        return SD::MustAppend;
    if (has(INDENT_CLOSE, rl))
        return SD::MustWrap;
    if (rl == "else") {
        if (ll == "end")
            return opts.statement.wrap_end_else_clauses ? SD::MustWrap : SD::MustAppend;
        if (lx == "}")
            return SD::MustAppend;
        return SD::MustWrap;
    }
    if (ll == "else" && rl == "begin")
        return SD::MustAppend;
    if (lx == ")" && rl == "begin")
        return SD::MustAppend;
    if (lx == "#")
        return SD::MustAppend;
    return SD::Undecided;
}

// ---------------------------------------------------------------------------
// Keyword case transform
// ---------------------------------------------------------------------------

static std::string kw_case(const std::string& t, const std::string& mode) {
    if (mode == "lower") {
        auto r = t;
        for (auto& c : r)
            c = (char)std::tolower((unsigned char)c);
        return r;
    }
    if (mode == "upper") {
        auto r = t;
        for (auto& c : r)
            c = (char)std::toupper((unsigned char)c);
        return r;
    }
    return t;
}

static void push_tok(std::vector<Tok>& toks, TokenKind kind, std::string text, int pos,
                     bool whitespace = false, bool comment = false, bool directive = false) {
    Tok t;
    t.kind = kind;
    t.lo = lower(text);
    t.text = std::move(text);
    t.pos = pos;
    t.whitespace = whitespace;
    t.comment = comment;
    t.directive = directive;
    toks.push_back(std::move(t));
}

static void append_trivia_text(std::vector<Tok>& toks, const std::string& source, size_t start,
                               size_t end) {
    size_t i = start;
    while (i < end) {
        if (source[i] == '/' && i + 1 < end && source[i + 1] == '/') {
            size_t j = i + 2;
            while (j < end && source[j] != '\n')
                ++j;
            push_tok(toks, TokenKind::Unknown, source.substr(i, j - i), (int)i, false, true, false);
            i = j;
        } else if (source[i] == '/' && i + 1 < end && source[i + 1] == '*') {
            size_t j = i + 2;
            while (j + 1 < end && !(source[j] == '*' && source[j + 1] == '/'))
                ++j;
            if (j + 1 < end)
                j += 2;
            push_tok(toks, TokenKind::Unknown, source.substr(i, j - i), (int)i, false, true, false);
            i = j;
        } else if (source[i] == '`') {
            size_t line_start = i;
            while (line_start > start && source[line_start - 1] != '\n')
                --line_start;
            size_t first = line_start;
            while (first < i && (source[first] == ' ' || source[first] == '\t'))
                ++first;
            size_t name_end = i + 1;
            while (name_end < end && (std::isalnum((unsigned char)source[name_end]) ||
                                      source[name_end] == '_' || source[name_end] == '$'))
                ++name_end;
            std::string name = source.substr(i, name_end - i);
            static const std::unordered_set<std::string> DIRECTIVES = {
                "`include", "`define", "`ifdef", "`ifndef", "`elsif", "`else", "`endif"};
            if (first == i && has(DIRECTIVES, name)) {
                size_t j = i;
                do {
                    while (j < end && source[j] != '\n')
                        ++j;
                    if (j == 0 || source[j - 1] != '\\')
                        break;
                    if (j < end)
                        ++j;
                } while (j < end);
                push_tok(toks, TokenKind::Unknown, source.substr(i, j - i), (int)i, false, false,
                         true);
                i = j;
            } else {
                push_tok(toks, TokenKind::Identifier, std::move(name), (int)i);
                i = name_end;
            }
        } else if (source[i] == '"') {
            size_t j = i + 1;
            while (j < end) {
                if (source[j] == '\\' && j + 1 < end) {
                    j += 2;
                } else if (source[j] == '"') {
                    ++j;
                    break;
                } else {
                    ++j;
                }
            }
            push_tok(toks, TokenKind::StringLiteral, source.substr(i, j - i), (int)i);
            i = j;
        } else if (std::isspace((unsigned char)source[i])) {
            size_t j = i + 1;
            while (j < end && std::isspace((unsigned char)source[j]))
                ++j;
            push_tok(toks, TokenKind::Unknown, source.substr(i, j - i), (int)i, true, false, false);
            i = j;
        } else if (std::isalpha((unsigned char)source[i]) || source[i] == '_' || source[i] == '$') {
            size_t j = i + 1;
            while (j < end &&
                   (std::isalnum((unsigned char)source[j]) || source[j] == '_' || source[j] == '$'))
                ++j;
            push_tok(toks, TokenKind::Identifier, source.substr(i, j - i), (int)i);
            i = j;
        } else if (std::isdigit((unsigned char)source[i])) {
            size_t j = i + 1;
            while (j < end && (std::isalnum((unsigned char)source[j]) || source[j] == '_' ||
                               source[j] == '\''))
                ++j;
            push_tok(toks, TokenKind::IntegerLiteral, source.substr(i, j - i), (int)i);
            i = j;
        } else if (std::ispunct((unsigned char)source[i])) {
            static const std::vector<std::string> OPS = {
                "<<=", ">>=", "===", "!==", "<<<", ">>>", "->", "<->", "&&", "||", "**", "##",
                "|->", "+=",  "-=",  "*=",  "/=",  "%=",  "&=", "|=",  "^=", "<=", ">=", "==",
                "!=",  "::",  "+:",  "-:",  "~&",  "~|",  "~^", "^~",  "++", "--", "<<", ">>"};
            bool matched = false;
            for (const auto& op : OPS) {
                if (i + op.size() <= end && source.compare(i, op.size(), op) == 0) {
                    push_tok(toks, TokenKind::Unknown, op, (int)i);
                    i += op.size();
                    matched = true;
                    break;
                }
            }
            if (!matched) {
                push_tok(toks, TokenKind::Unknown, source.substr(i, 1), (int)i);
                ++i;
            }
        } else {
            push_tok(toks, TokenKind::Unknown, source.substr(i, 1), (int)i);
            ++i;
        }
    }
}

static std::string normalize_spacing_fragment(const std::string& text, const FormatOptions& opts,
                                              bool in_dim) {
    std::vector<Tok> toks;
    append_trivia_text(toks, text, 0, text.size());

    std::string out;
    const Tok* prev = nullptr;
    for (const auto& tok : toks) {
        if (tok.whitespace)
            continue;
        if (prev) {
            int spaces =
                spaces_req(*prev, tok, opts, in_dim, false, false, ParenKind::Ordinary, false);
            if (spaces > 0)
                out += std::string(spaces, ' ');
        }
        out += tok.text;
        prev = &tok;
    }
    return out;
}

static std::string normalize_bracket_spacing(const std::string& text, const FormatOptions& opts) {
    std::string out;
    for (size_t i = 0; i < text.size();) {
        if (text[i] != '[') {
            out += text[i++];
            continue;
        }

        size_t start = i;
        int depth = 0;
        do {
            if (text[i] == '[')
                ++depth;
            else if (text[i] == ']')
                --depth;
            ++i;
        } while (i < text.size() && depth > 0);
        out += normalize_spacing_fragment(text.substr(start, i - start), opts, true);
    }
    return out;
}

static std::vector<Tok> collect_cst_tokens(const std::string& source, const SyntaxTree& tree,
                                           bool source_only) {
    std::vector<Tok> toks;
    size_t cursor = 0;
    const auto& sm = tree.sourceManager();
    for (auto it = tree.root().tokens_begin(); it != tree.root().tokens_end(); ++it) {
        auto token = *it;
        if (!token || token.isMissing() || token.kind == TokenKind::EndOfFile)
            continue;
        auto loc = token.location();
        if (!loc.valid())
            continue;
        if (!sm.isFileLoc(loc))
            continue;
        if (source_only) {
            auto buf_text = sm.getSourceText(loc.buffer());
            if (buf_text.size() < source.size() ||
                buf_text.substr(0, source.size()) != std::string_view(source))
                continue;
        }
        size_t off = loc.offset();
        if (off > source.size())
            continue;
        if (off < cursor)
            continue;
        if (cursor < off)
            append_trivia_text(toks, source, cursor, off);
        std::string raw(token.rawText());
        if (!raw.empty())
            push_tok(toks, token.kind, raw, (int)off);
        cursor = std::max(cursor, off + raw.size());
    }
    if (cursor < source.size())
        append_trivia_text(toks, source, cursor, source.size());
    return toks;
}

// ---------------------------------------------------------------------------
// Split by top-level comma (depth-0)
// ---------------------------------------------------------------------------

static std::vector<std::string> split_top_level(const std::string& text) {
    std::vector<std::string> parts;
    int depth = 0;
    size_t start = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (c == '(' || c == '[' || c == '{')
            ++depth;
        else if (c == ')' || c == ']' || c == '}')
            --depth;
        else if (c == ',' && depth == 0) {
            parts.push_back(text.substr(start, i - start));
            start = i + 1;
        }
    }
    parts.push_back(text.substr(start));
    return parts;
}

// ---------------------------------------------------------------------------
// Port-declaration alignment pass
// ---------------------------------------------------------------------------

static const std::unordered_set<std::string> PORT_DIRS = {"input", "output", "inout"};

struct PortParsed {
    bool valid{false};
    std::string indent, direction, dtype, qualifier, dim;
    std::vector<std::pair<std::string, std::string>> names; // (name, trailing)
    std::string terminator, comment;
};

static PortParsed parse_port(const std::string& raw, const FormatOptions& opts) {
    PortParsed r;
    std::string s = raw;
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
        s.pop_back();
    size_t p = 0;
    while (p < s.size() && (s[p] == ' ' || s[p] == '\t'))
        ++p;
    r.indent = s.substr(0, p);
    std::string code = s.substr(p);

    auto cp = code.find("//");
    if (cp != std::string::npos) {
        r.comment = " " + code.substr(cp);
        code = code.substr(0, cp);
        while (!code.empty() && (code.back() == ' ' || code.back() == '\t'))
            code.pop_back();
    }
    if (!code.empty() && (code.back() == ',' || code.back() == ';')) {
        r.terminator = std::string(1, code.back());
        code.pop_back();
        while (!code.empty() && (code.back() == ' ' || code.back() == '\t'))
            code.pop_back();
    }

    std::vector<std::string> toks;
    {
        std::vector<std::string> raw;
        std::istringstream ss(code);
        std::string w;
        while (ss >> w)
            raw.push_back(w);
        // Expand compact "ident[...]" tokens (e.g., "packet_t[0:0]" → "packet_t" + "[0:0]")
        // so parse_port correctly identifies user-defined types with packed dimensions.
        for (auto& t : raw) {
            if (!t.empty() && t[0] != '[' && (std::isalpha((unsigned char)t[0]) || t[0] == '_')) {
                auto bk = t.find('[');
                if (bk != std::string::npos) {
                    toks.push_back(t.substr(0, bk));
                    toks.push_back(t.substr(bk));
                    continue;
                }
            }
            toks.push_back(t);
        }
    }
    if (toks.empty())
        return r;

    static const std::unordered_set<std::string> BTYPES = {
        "logic", "wire",    "reg",     "bit",   "byte",     "shortint",
        "int",   "longint", "integer", "real",  "realtime", "shortreal",
        "time",  "string",  "chandle", "event", "var",
    };
    static const std::unordered_set<std::string> NET_TYPES = {
        "var",    "wire", "uwire", "tri",    "tri0",    "tri1",    "wand",
        "triand", "wor",  "trior", "trireg", "supply0", "supply1",
    };
    static const std::unordered_set<std::string> DATA_TYPES = {
        "logic", "reg", "bit", "byte", "shortint", "int", "longint", "integer", "time",
    };
    static const std::unordered_set<std::string> QUALS = {"signed", "unsigned"};

    std::string d0 = lower(toks[0]);
    if (!has(PORT_DIRS, d0))
        return r;
    r.direction = toks[0];
    size_t idx = 1;

    // Check if string is a pure identifier (no commas or special chars)
    auto is_pure_id = [](const std::string& s) -> bool {
        if (s.empty() || (!std::isalpha((unsigned char)s[0]) && s[0] != '_'))
            return false;
        for (size_t k = 1; k < s.size(); ++k) {
            char c = s[k];
            if (c == ':' && k + 1 < s.size() && s[k + 1] == ':') {
                k += 1;
                continue;
            }
            if (!std::isalnum((unsigned char)c) && c != '_')
                return false;
        }
        return true;
    };

    if (idx < toks.size()) {
        const std::string& cand = toks[idx];
        std::string cl = lower(cand);
        bool pure_id = is_pure_id(cand);
        if (pure_id && cand[0] != '[' && !has(QUALS, cl)) {
            bool is_builtin = has(BTYPES, cl);
            bool is_usertype = pure_id && idx + 1 < toks.size();
            if (is_builtin || is_usertype) {
                r.dtype = toks[idx++];
                if (has(NET_TYPES, cl) && idx < toks.size()) {
                    std::string ncl = lower(toks[idx]);
                    if (has(DATA_TYPES, ncl)) {
                        r.dtype += " " + toks[idx++];
                    }
                }
            }
        }
    }
    if (idx < toks.size() && has(QUALS, lower(toks[idx])))
        r.qualifier = toks[idx++];
    if (idx < toks.size() && !toks[idx].empty() && toks[idx][0] == '[') {
        int depth = 0;
        while (idx < toks.size()) {
            auto& t = toks[idx];
            r.dim += t;
            for (char ch : t) {
                if (ch == '[')
                    ++depth;
                else if (ch == ']')
                    --depth;
            }
            ++idx;
            if (depth <= 0)
                break;
        }
    }
    if (idx >= toks.size())
        return r;

    // Remaining tokens are comma-separated names (possibly with unpacked dims/defaults)
    // Rebuild remaining as string and split by top-level comma
    std::string remaining;
    for (size_t k = idx; k < toks.size(); ++k) {
        if (k > idx)
            remaining += ' ';
        remaining += toks[k];
    }
    auto raw_names = split_top_level(remaining);
    if (raw_names.empty())
        return r;

    // Split "name trailing..." into (name, trailing)
    auto split_id_trail = [](const std::string& nm) -> std::pair<std::string, std::string> {
        size_t i = 0;
        while (i < nm.size() && (std::isalnum((unsigned char)nm[i]) || nm[i] == '_'))
            ++i;
        std::string name = nm.substr(0, i);
        while (i < nm.size() && (nm[i] == ' ' || nm[i] == '\t'))
            ++i;
        return {name, nm.substr(i)};
    };

    for (auto& rn : raw_names) {
        // trim
        size_t a = 0;
        while (a < rn.size() && (rn[a] == ' ' || rn[a] == '\t'))
            ++a;
        size_t b = rn.size();
        while (b > a && (rn[b - 1] == ' ' || rn[b - 1] == '\t'))
            --b;
        std::string nm = rn.substr(a, b - a);
        auto [name, trail] = split_id_trail(nm);
        if (!name.empty()) {
            r.names.push_back({name, normalize_bracket_spacing(trail, opts)});
        } else {
            r.names.push_back({nm, ""});
        }
    }
    r.dim = normalize_bracket_spacing(r.dim, opts);
    r.valid = !r.names.empty();
    return r;
}

static std::vector<FormattedLine> align_port_pass(std::vector<FormattedLine> lines,
                                                   const FormatOptions& opts) {
    std::vector<FormattedLine> out;
    size_t i = 0;
    bool disabled_region = false;
    bool module_header_region = false;
    auto starts_with_port_dir = [](const std::string& line) -> bool {
        size_t p = 0;
        while (p < line.size() && (line[p] == ' ' || line[p] == '\t'))
            ++p;
        size_t q = p;
        while (q < line.size() && std::isalpha((unsigned char)line[q]))
            ++q;
        return has(PORT_DIRS, lower(line.substr(p, q - p)));
    };
    auto is_port_decl_line = [&](int idx) -> bool {
        if (idx < 0 || idx >= (int)lines.size())
            return false;
        if (starts_with_port_dir(lines[idx].text))
            return true;
        return lines[(size_t)idx].kind == LineKind::PortDecl;
    };
    auto is_semi_only = [&](int idx) -> bool {
        if (idx < 0 || idx >= (int)lines.size())
            return false;
        const std::string& line = lines[idx].text;
        size_t p = 0;
        while (p < line.size() && (line[p] == ' ' || line[p] == '\t'))
            ++p;
        return p < line.size() && line[p] == ';' &&
               line.find_first_not_of(" \t", p + 1) == std::string::npos;
    };
    while (i < lines.size()) {
        std::string trimmed_line = lines[i].text;
        size_t ta = trimmed_line.find_first_not_of(" \t");
        size_t tb = trimmed_line.find_last_not_of(" \t");
        trimmed_line =
            (ta == std::string::npos) ? std::string() : trimmed_line.substr(ta, tb - ta + 1);
        if (module_header_region) {
            out.push_back(lines[i]);
            if (trimmed_line.find(");") != std::string::npos)
                module_header_region = false;
            ++i;
            continue;
        }
        if ((trimmed_line.rfind("module ", 0) == 0 || trimmed_line.rfind("macromodule ", 0) == 0) &&
            trimmed_line.find('(') != std::string::npos &&
            trimmed_line.find(';') == std::string::npos) {
            module_header_region = true;
            out.push_back(lines[i]);
            ++i;
            continue;
        }
        if (trimmed_line.find("verilog_format") != std::string::npos &&
            trimmed_line.find("off") != std::string::npos) {
            disabled_region = true;
            out.push_back(lines[i]);
            ++i;
            continue;
        }
        if (disabled_region) {
            out.push_back(lines[i]);
            if (trimmed_line.find("verilog_format") != std::string::npos &&
                trimmed_line.find("on") != std::string::npos)
                disabled_region = false;
            ++i;
            continue;
        }
        if (!is_port_decl_line((int)i)) {
            out.push_back(lines[i]);
            ++i;
            continue;
        }

        struct PortBlkEntry {
            FormattedLine orig;
            std::string port_text;
            PortParsed parsed;
        };
        std::vector<PortBlkEntry> blk;
        size_t j = i;
        while (j < lines.size()) {
            if (!is_port_decl_line((int)j))
                break;
            FormattedLine fl = lines[j];
            std::string port_line = lines[j].text;
            ++j;
            if (is_semi_only((int)j)) {
                port_line += ";";
                ++j;
            }
            blk.push_back({fl, port_line, parse_port(port_line, opts)});
        }

        int md = 0, ms2_content = 0, mdim = 0;
        int np = 0;
        size_t max_slots = 0;
        for (auto& e : blk) {
            if (!e.parsed.valid)
                continue;
            ++np;
            md = std::max(md, (int)e.parsed.direction.size());
            std::string s2 = e.parsed.dtype + (e.parsed.qualifier.empty() ? "" : " " + e.parsed.qualifier);
            ms2_content = std::max(ms2_content, (int)s2.size());
            mdim = std::max(mdim, (int)e.parsed.dim.size());
            max_slots = std::max(max_slots, e.parsed.names.size());
        }
        if (!np) {
            for (auto& e : blk)
                out.push_back(e.orig);
            i = j;
            continue;
        }

        const auto& pd = opts.port_declaration;
        int pd_s1_min = tab_aligned_width(pd.section1_min_width, opts);
        int pd_s2_min = tab_aligned_width(pd.section2_min_width, opts);
        int pd_s3_min = tab_aligned_width(pd.section3_min_width, opts);
        int pd_s4_min = tab_aligned_width(pd.section4_min_width, opts);
        int pd_s5_min = tab_aligned_width(pd.section5_min_width, opts);

        int s1 = tab_aligned_width(std::max(pd_s1_min, md + 1), opts);
        int s2 =
            ms2_content > 0 ? tab_aligned_width(std::max(pd_s2_min, ms2_content + 1), opts) : 0;
        int s3 = mdim > 0 ? tab_aligned_width(std::max(pd_s3_min, mdim + 1), opts) : 0;
        int s5_min = pd_s5_min;

        // Per-slot id and trailing widths
        std::vector<int> id_widths(max_slots, 0), trail_widths(max_slots, 0);
        for (size_t slot = 0; slot < max_slots; ++slot) {
            int max_id = 0, max_tr = 0;
            for (auto& e : blk) {
                if (!e.parsed.valid)
                    continue;
                if (slot < e.parsed.names.size()) {
                    max_id = std::max(max_id, (int)e.parsed.names[slot].first.size());
                    max_tr = std::max(max_tr, (int)e.parsed.names[slot].second.size());
                }
            }
            id_widths[slot] = tab_aligned_width(std::max(pd_s4_min, max_id + 1), opts);
            trail_widths[slot] = tab_aligned_width(std::max(s5_min, max_tr), opts);
        }

        auto pad = [](std::string s, int w) -> std::string {
            s.resize(std::max((int)s.size(), w), ' ');
            return s;
        };

        for (auto& e : blk) {
            const auto& pp = e.parsed;
            if (!pp.valid) {
                out.push_back(e.orig);
                continue;
            }
            int line_s1 = s1;
            int line_s2 = s2;
            int line_s3 = s3;
            std::vector<int> line_id_widths = id_widths;
            std::vector<int> line_trail_widths = trail_widths;
            if (opts.port_declaration.align_adaptive) {
                std::string tp = pp.dtype + (pp.qualifier.empty() ? "" : " " + pp.qualifier);
                // Cumulative minimum target end columns (fixed reference points).
                // Each section pads to max(target_end, actual_end).
                // Overflow on one line is absorbed by subsequent sections' slack,
                // so downstream sections stay aligned as long as overflow is small.
                int t1 = pd_s1_min;
                int t2 = t1 + (s2 > 0 ? pd_s2_min : 0);
                int t3 = t2 + (s3 > 0 ? pd_s3_min : 0);

                int e1 = tab_aligned_width(std::max(t1, (int)pp.direction.size() + 1), opts);
                line_s1 = e1;

                int e2 = e1;
                if (s2 > 0) {
                    int c2 = tp.empty() ? 0 : (int)tp.size() + 1;
                    e2 = tab_aligned_width(std::max(t2, e1 + c2), opts);
                    line_s2 = e2 - e1;
                }

                if (s3 > 0) {
                    int c3 = pp.dim.empty() ? 0 : (int)pp.dim.size() + 1;
                    int e3 = tab_aligned_width(std::max(t3, e2 + c3), opts);
                    line_s3 = e3 - e2;
                }

                line_id_widths.clear();
                line_trail_widths.clear();
                for (const auto& [nm, tr] : pp.names) {
                    line_id_widths.push_back(
                        tab_aligned_width(std::max(pd_s4_min, (int)nm.size() + 1), opts));
                    line_trail_widths.push_back(
                        tab_aligned_width(std::max(s5_min, (int)tr.size()), opts));
                }
            }
            std::string line = pp.indent;
            line += pad(pp.direction, line_s1);
            if (line_s2 > 0) {
                std::string tp = pp.dtype + (pp.qualifier.empty() ? "" : " " + pp.qualifier);
                line += pad(tp, line_s2);
            }
            if (line_s3 > 0)
                line += pad(pp.dim, line_s3);

            // Emit per-slot names — mirrors Python _reassemble_port_line
            size_t nslots = pp.names.size();
            for (size_t slot = 0; slot < nslots; ++slot) {
                bool is_last = (slot == nslots - 1);
                const auto& nm = pp.names[slot].first;
                const auto& tr = pp.names[slot].second;
                // Pad name to slot id width
                if (slot < line_id_widths.size())
                    line += pad(nm, line_id_widths[slot]);
                else
                    line += nm;

                if (!is_last) {
                    // Non-last slot: pad trailing then ", "
                    if (!tr.empty() && s5_min > 0 && slot < line_trail_widths.size() &&
                        line_trail_widths[slot] > 1)
                        line += pad(tr, line_trail_widths[slot]) + ", ";
                    else if (slot < line_trail_widths.size())
                        line += pad(tr, line_trail_widths[slot]) + ", ";
                    else
                        line += tr + ", ";
                } else {
                    // Last slot — matches Python _reassemble_port_line last-slot logic
                    std::string term = pp.terminator.empty() ? ";" : pp.terminator;
                    if (!tr.empty() && s5_min > 0 && slot < line_trail_widths.size() &&
                        line_trail_widths[slot] > 1) {
                        line += pad(tr, line_trail_widths[slot]) + term;
                    } else {
                        if (slot < line_trail_widths.size())
                            line += pad(tr, line_trail_widths[slot]);
                        else
                            line += tr;
                        line += term;
                    }
                }
            }
            if (!pp.comment.empty())
                line += pp.comment;
            while (!line.empty() && line.back() == ' ')
                line.pop_back();
            out.push_back({line, e.orig.kind});
        }
        i = j;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Statement assignment alignment pass
// ---------------------------------------------------------------------------

static std::vector<FormattedLine> align_assign_pass(std::vector<FormattedLine> lines,
                                                     const FormatOptions& opts) {
    auto find_op = [&](const std::string& line) -> std::pair<int, std::string> {
        size_t cp = line.find("//");
        std::string code = (cp != std::string::npos) ? line.substr(0, cp) : line;
        static const std::vector<std::string> OPS = {"<<<=", ">>>=", "<<=", ">>=", "<=", "+=", "-=",
                                                     "*=",   "/=",   "%=",  "&=",  "|=", "^=", "="};
        int paren = 0;
        int bracket = 0;
        int brace = 0;
        for (size_t i = 0; i < code.size(); ++i) {
            char ch = code[i];
            if (ch == '(')
                ++paren;
            else if (ch == ')' && paren > 0)
                --paren;
            else if (ch == '[')
                ++bracket;
            else if (ch == ']' && bracket > 0)
                --bracket;
            else if (ch == '{')
                ++brace;
            else if (ch == '}' && brace > 0)
                --brace;
            if (paren != 0 || bracket != 0 || brace != 0 || ch != ' ')
                continue;
            for (const auto& op : OPS) {
                size_t op_pos = i + 1;
                size_t after = op_pos + op.size();
                if (after < code.size() && code.compare(op_pos, op.size(), op) == 0 &&
                    code[after] == ' ') {
                    return {(int)i, op};
                }
            }
        }
        return {-1, ""};
    };

    auto is_var = [&](int line_idx) -> bool {
        return lines[line_idx].kind == LineKind::VarDecl;
    };

    std::vector<FormattedLine> out;
    size_t i = 0;
    while (i < lines.size()) {
        auto [p0, op0] = find_op(lines[i].text);
        if (p0 < 0 || is_var((int)i)) {
            out.push_back(lines[i]);
            ++i;
            continue;
        }
        size_t ind = 0;
        while (ind < lines[i].text.size() &&
               (lines[i].text[ind] == ' ' || lines[i].text[ind] == '\t'))
            ++ind;

        struct E {
            std::string line;
            int pos;
            std::string op;
            int lw;
            LineKind kind;
        };
        std::vector<E> grp;
        size_t j = i;
        while (j < lines.size()) {
            const auto& lj = lines[j].text;
            if (lj.empty())
                break;
            if (is_var((int)j))
                break;
            size_t ij = 0;
            while (ij < lj.size() && (lj[ij] == ' ' || lj[ij] == '\t'))
                ++ij;
            if (ij != ind)
                break;
            auto [pj, oj] = find_op(lj);
            if (pj < 0)
                break;
            grp.push_back({lj, pj, oj, (int)(pj - (int)ij), lines[j].kind});
            ++j;
        }
        if (grp.empty()) {
            out.push_back(lines[i]);
            ++i;
            continue;
        }

        int mx = opts.statement.lhs_min_width;
        for (auto& e : grp)
            mx = std::max(mx, e.lw);
        int col = mx + 1;
        if (opts.tab_align && opts.indent_size > 0)
            col = snap_to_indent_grid((int)ind + mx + 1, opts.indent_size) - (int)ind;
        for (auto& e : grp) {
            int sp = std::max(1, col - e.lw);
            std::string lhs = e.line.substr(0, e.pos);
            size_t rs = (size_t)(e.pos + 1 + (int)e.op.size() + 1);
            std::string rhs = (rs < e.line.size()) ? e.line.substr(rs) : "";
            out.push_back({lhs + std::string(sp, ' ') + e.op + " " + rhs, e.kind});
        }
        i = j;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Variable declaration alignment pass — ported from _align_variable_declarations_pass
// ---------------------------------------------------------------------------

static const std::unordered_set<std::string> VAR_BUILTIN_TYPES = {
    "wire",    "logic", "reg",      "bit",     "byte",   "int",
    "integer", "time",  "shortint", "longint", "signed", "unsigned",
};
static const std::unordered_set<std::string> VAR_PREFIX_KW = {
    "static",
    "automatic",
    "const",
    "var",
};
static const std::unordered_set<std::string> VAR_EXCLUDED = {
    "input",
    "output",
    "inout",
    "ref",
};

struct VarParsed {
    std::string indent, type_kw, qualifier, dim;
    std::vector<std::pair<std::string, std::string>> declarators; // (name, trailing)
    std::string comment;
};

static VarParsed* parse_var_line(const std::string& line, const FormatOptions& opts) {
    std::string stripped = line;
    while (!stripped.empty() && (stripped.back() == ' ' || stripped.back() == '\t'))
        stripped.pop_back();
    size_t ip = 0;
    while (ip < stripped.size() && (stripped[ip] == ' ' || stripped[ip] == '\t'))
        ++ip;
    std::string indent = stripped.substr(0, ip);
    std::string code = stripped.substr(ip);

    // Strip trailing // comment
    std::string comment;
    auto cm = code.rfind("//");
    if (cm != std::string::npos) {
        // Make sure // is not inside a string
        comment = " " + code.substr(cm);
        code = code.substr(0, cm);
        while (!code.empty() && (code.back() == ' ' || code.back() == '\t'))
            code.pop_back();
    }

    // Must end with ;
    if (code.empty() || code.back() != ';')
        return nullptr;
    code.pop_back();
    while (!code.empty() && (code.back() == ' ' || code.back() == '\t'))
        code.pop_back();

    // Split into tokens, expanding compact "identifier[...]" tokens
    std::vector<std::string> raw_toks;
    {
        std::istringstream ss(code);
        std::string w;
        while (ss >> w)
            raw_toks.push_back(w);
    }
    if (raw_toks.empty())
        return nullptr;
    // Expand compact "ident[...]" -> "ident" + "[...]"
    std::vector<std::string> toks;
    for (auto& t : raw_toks) {
        if (!t.empty() && t[0] != '[' && (std::isalpha((unsigned char)t[0]) || t[0] == '_')) {
            auto bk = t.find('[');
            if (bk != std::string::npos) {
                toks.push_back(t.substr(0, bk));
                toks.push_back(t.substr(bk));
                continue;
            }
        }
        toks.push_back(t);
    }

    std::string first = lower(toks[0]);
    if (has(VAR_EXCLUDED, first))
        return nullptr;

    size_t idx = 0;
    std::vector<std::string> type_parts;
    while (idx < toks.size() && has(VAR_PREFIX_KW, lower(toks[idx]))) {
        type_parts.push_back(toks[idx++]);
    }
    if (idx >= toks.size())
        return nullptr;

    first = lower(toks[idx]);
    if (has(VAR_BUILTIN_TYPES, first)) {
        type_parts.push_back(toks[idx++]);
    } else {
        // User-defined type: must be an identifier not an SV keyword
        if (!std::isalpha((unsigned char)toks[idx][0]) && toks[idx][0] != '_')
            return nullptr;
        if (has(SV_KW, first))
            return nullptr;
        if (idx + 1 >= toks.size())
            return nullptr;
        // Next must look like dimension, qualifier, or identifier
        const std::string& nxt = toks[idx + 1];
        std::string nxtl = lower(nxt);
        static const std::unordered_set<std::string> QUALS2 = {"signed", "unsigned"};
        bool ok = nxt[0] == '[' || std::isalpha((unsigned char)nxt[0]) || nxt[0] == '_' ||
                  has(QUALS2, nxtl);
        if (!ok)
            return nullptr;
        type_parts.push_back(toks[idx++]);
    }

    std::string type_kw;
    for (size_t k = 0; k < type_parts.size(); ++k) {
        if (k)
            type_kw += ' ';
        type_kw += type_parts[k];
    }

    // Optional qualifier
    std::string qualifier;
    static const std::unordered_set<std::string> QUALS = {"signed", "unsigned"};
    if (idx < toks.size() && has(QUALS, lower(toks[idx])))
        qualifier = toks[idx++];

    // Optional packed dim
    std::string dim;
    if (idx < toks.size() && !toks[idx].empty() && toks[idx][0] == '[') {
        int depth = 0;
        while (idx < toks.size()) {
            dim += toks[idx];
            for (char c : toks[idx]) {
                if (c == '[')
                    ++depth;
                else if (c == ']')
                    --depth;
            }
            ++idx;
            if (depth <= 0)
                break;
        }
    }
    if (idx >= toks.size())
        return nullptr;

    // Remaining: comma-separated declarators
    std::string remaining;
    for (size_t k = idx; k < toks.size(); ++k) {
        if (k > idx)
            remaining += ' ';
        remaining += toks[k];
    }

    auto raw_names = split_top_level(remaining);
    if (raw_names.empty())
        return nullptr;

    // Validate: each name must start with [A-Za-z_]
    for (auto& rn : raw_names) {
        size_t a = 0;
        while (a < rn.size() && (rn[a] == ' ' || rn[a] == '\t'))
            ++a;
        if (a >= rn.size())
            continue;
        char fc = rn[a];
        if (!std::isalpha((unsigned char)fc) && fc != '_')
            return nullptr;
    }

    auto* vp = new VarParsed{indent, type_kw, qualifier, normalize_bracket_spacing(dim, opts),
                             {},     comment};
    for (auto& rn : raw_names) {
        size_t a = 0;
        while (a < rn.size() && (rn[a] == ' ' || rn[a] == '\t'))
            ++a;
        size_t b = rn.size();
        while (b > a && (rn[b - 1] == ' ' || rn[b - 1] == '\t'))
            --b;
        std::string nm = rn.substr(a, b - a);
        size_t ni = 0;
        while (ni < nm.size() && (std::isalnum((unsigned char)nm[ni]) || nm[ni] == '_'))
            ++ni;
        std::string dname = nm.substr(0, ni);
        while (ni < nm.size() && (nm[ni] == ' ' || nm[ni] == '\t'))
            ++ni;
        vp->declarators.push_back({dname, normalize_bracket_spacing(nm.substr(ni), opts)});
    }
    if (vp->declarators.empty()) {
        delete vp;
        return nullptr;
    }
    // Reject if any declarator looks like a function/instance call (has '(' in name or trailing)
    for (auto& decl : vp->declarators) {
        if (decl.first.find('(') != std::string::npos ||
            decl.second.find('(') != std::string::npos) {
            delete vp;
            return nullptr;
        }
    }
    return vp;
}

static std::vector<FormattedLine> align_var_pass(std::vector<FormattedLine> lines,
                                                  const FormatOptions& opts) {
    const auto& vo = opts.var_declaration;

    auto is_var_idx = [&](int idx) -> bool {
        if (lines[(size_t)idx].kind == LineKind::VarDecl)
            return true;
        VarParsed* parsed = parse_var_line(lines[(size_t)idx].text, opts);
        if (!parsed)
            return false;
        delete parsed;
        return true;
    };

    std::vector<FormattedLine> out;
    size_t i = 0;
    while (i < lines.size()) {
        // Enter a block only if this line is a variable declaration.
        if (!is_var_idx((int)i)) {
            out.push_back(lines[i]);
            ++i;
            continue;
        }

        // Collect block
        struct BlkEntry {
            FormattedLine orig;
            VarParsed* parsed;
        };
        std::vector<BlkEntry> block;
        size_t j = i;
        while (j < lines.size()) {
            const std::string& cur = lines[j].text;
            if (is_var_idx((int)j)) {
                block.push_back({lines[j], parse_var_line(cur, opts)});
                ++j;
                continue;
            }
            // Comment/blank lines pass through without breaking block
            size_t sp = 0;
            while (sp < cur.size() && (cur[sp] == ' ' || cur[sp] == '\t'))
                ++sp;
            std::string trimmed = cur.substr(sp);
            if (trimmed.empty() || trimmed.substr(0, 2) == "//" ||
                (trimmed.size() >= 2 && trimmed[0] == '/' && trimmed[1] == '*')) {
                block.push_back({lines[j], nullptr});
                ++j;
                continue;
            }
            break;
        }

        // Count parseable entries
        int np = 0;
        for (auto& e : block)
            if (e.parsed)
                ++np;

        if (np <= 0) {
            for (auto& e : block) {
                out.push_back(e.orig);
                if (e.parsed)
                    delete e.parsed;
            }
            i = j;
            continue;
        }

        // Compute section widths
        int max_s1 = 0;
        for (auto& e : block) {
            if (!e.parsed)
                continue;
            std::string s1 =
                e.parsed->type_kw + (e.parsed->qualifier.empty() ? "" : " " + e.parsed->qualifier);
            max_s1 = std::max(max_s1, (int)s1.size());
        }
        int vo_s1_min = tab_aligned_width(vo.section1_min_width, opts);
        int vo_s2_min = tab_aligned_width(vo.section2_min_width, opts);
        int vo_s3_min = tab_aligned_width(vo.section3_min_width, opts);
        int vo_s4_min = tab_aligned_width(vo.section4_min_width, opts);

        int s1_w = tab_aligned_width(std::max(vo_s1_min, max_s1 + 1), opts);

        int max_dim = 0;
        for (auto& e : block)
            if (e.parsed)
                max_dim = std::max(max_dim, (int)e.parsed->dim.size());
        int s2_w = max_dim > 0 ? tab_aligned_width(std::max(vo_s2_min, max_dim + 1), opts) : 0;

        size_t max_slots = 0;
        for (auto& e : block)
            if (e.parsed)
                max_slots = std::max(max_slots, e.parsed->declarators.size());

        std::vector<int> id_widths(max_slots, 0), trail_widths(max_slots, 0);
        for (size_t slot = 0; slot < max_slots; ++slot) {
            int mx_id = 0, mx_tr = 0;
            for (auto& e : block) {
                if (!e.parsed)
                    continue;
                if (slot < e.parsed->declarators.size()) {
                    mx_id = std::max(mx_id, (int)e.parsed->declarators[slot].first.size());
                    mx_tr = std::max(mx_tr, (int)e.parsed->declarators[slot].second.size());
                }
            }
            id_widths[slot] = tab_aligned_width(std::max(vo_s3_min, mx_id + 1), opts);
            trail_widths[slot] = tab_aligned_width(std::max(vo_s4_min, mx_tr), opts);
        }

        auto pad_to = [](std::string s, int w) -> std::string {
            if ((int)s.size() < w)
                s.resize(w, ' ');
            return s;
        };

        for (auto& e : block) {
            if (!e.parsed) {
                out.push_back(e.orig);
                continue;
            }
            const auto& vp = *e.parsed;
            int line_s1_w = s1_w;
            int line_s2_w = s2_w;
            std::vector<int> line_id_widths = id_widths;
            std::vector<int> line_trail_widths = trail_widths;
            if (vo.align_adaptive) {
                std::string s1part = vp.type_kw + (vp.qualifier.empty() ? "" : " " + vp.qualifier);
                // Cumulative minimum target end columns — same model as port_declaration.
                int t1 = vo_s1_min;
                int t2 = t1 + (s2_w > 0 ? vo_s2_min : 0);

                int e1 = tab_aligned_width(std::max(t1, (int)s1part.size() + 1), opts);
                line_s1_w = e1;

                int e2 = e1;
                if (s2_w > 0) {
                    int c2 = vp.dim.empty() ? 0 : (int)vp.dim.size() + 1;
                    e2 = tab_aligned_width(std::max(t2, e1 + c2), opts);
                    line_s2_w = e2 - e1;
                }

                // Keep declarator/trailing widths block-wide so a name or initializer that
                // overflows its minimum section still moves following semicolons consistently for
                // the block.
                line_trail_widths.clear();
                for (const auto& [_, tr] : vp.declarators)
                    line_trail_widths.push_back(
                        tab_aligned_width(std::max(vo_s4_min, (int)tr.size()), opts));
            }
            std::string ln = vp.indent;
            std::string s1part = vp.type_kw + (vp.qualifier.empty() ? "" : " " + vp.qualifier);
            ln += pad_to(s1part, line_s1_w);
            if (line_s2_w > 0)
                ln += pad_to(vp.dim, line_s2_w);
            size_t nd = vp.declarators.size();
            for (size_t k = 0; k < nd; ++k) {
                bool is_last = (k == nd - 1);
                const auto& nm = vp.declarators[k].first;
                const auto& tr = vp.declarators[k].second;
                if (!is_last) {
                    ln += pad_to(nm, line_id_widths[k]);
                    if (!tr.empty() && vo.section4_min_width > 0 && k < line_trail_widths.size() &&
                        line_trail_widths[k] > 1) {
                        ln += pad_to(tr, line_trail_widths[k]) + ", ";
                    } else if (k < line_trail_widths.size()) {
                        ln += pad_to(tr, line_trail_widths[k]) + ", ";
                    } else {
                        ln += tr + ", ";
                    }
                } else {
                    // Last slot — mirrors Python _reassemble_var_line last-slot logic
                    if (k < line_id_widths.size())
                        ln += pad_to(nm, line_id_widths[k]);
                    else
                        ln += nm;
                    if (!tr.empty() && vo.section4_min_width > 0 && k < line_trail_widths.size() &&
                        line_trail_widths[k] > 1) {
                        ln += pad_to(tr, line_trail_widths[k]) + ";";
                    } else {
                        if (k < line_trail_widths.size())
                            ln += pad_to(tr, line_trail_widths[k]);
                        ln += tr + ";";
                    }
                }
            }
            if (!vp.comment.empty())
                ln += vp.comment;
            while (!ln.empty() && ln.back() == ' ')
                ln.pop_back();
            out.push_back({ln, e.orig.kind});
            delete e.parsed;
        }
        i = j;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Module instantiation expansion pass
// ---------------------------------------------------------------------------

struct InstanceComments {
    std::string header;
    std::vector<std::pair<std::string, std::string>> port_comments;
};

static size_t find_line_comment_start(const std::string& line) {
    bool in_string = false;
    bool escaped = false;
    for (size_t i = 0; i + 1 < line.size(); ++i) {
        char ch = line[i];
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                in_string = false;
            }
            continue;
        }
        if (ch == '"') {
            in_string = true;
            continue;
        }
        if (ch == '/' && line[i + 1] == '/')
            return i;
    }
    return std::string::npos;
}

static std::string last_named_port_before_comment(const std::string& code) {
    std::string last;
    for (size_t i = 0; i < code.size(); ++i) {
        if (code[i] != '.')
            continue;
        size_t j = i + 1;
        if (j >= code.size() || !(std::isalpha((unsigned char)code[j]) || code[j] == '_'))
            continue;
        ++j;
        while (j < code.size() && (std::isalnum((unsigned char)code[j]) || code[j] == '_'))
            ++j;
        size_t k = j;
        while (k < code.size() && (code[k] == ' ' || code[k] == '\t'))
            ++k;
        if (k < code.size() && code[k] == '(')
            last = code.substr(i + 1, j - i - 1);
    }
    return last;
}

// Collect lines from start until ')' at depth 0 followed by ';'
static bool collect_instance(const std::vector<std::string>& lines, size_t start, size_t& end_i,
                             std::string& flat, InstanceComments& comments) {
    std::vector<std::string> parts;
    int depth = 0;
    size_t j = start;
    while (j < lines.size()) {
        std::string stripped = lines[j];
        size_t sp = 0;
        while (sp < stripped.size() && (stripped[sp] == ' ' || stripped[sp] == '\t'))
            ++sp;
        stripped = stripped.substr(sp);

        std::string code = stripped;
        size_t line_comment = find_line_comment_start(stripped);
        if (line_comment != std::string::npos) {
            code = stripped.substr(0, line_comment);
            std::string comment = stripped.substr(line_comment);
            std::string port = last_named_port_before_comment(code);
            if (!port.empty())
                comments.port_comments.push_back({port, comment});
            else if (code.find('(') != std::string::npos && comments.header.empty())
                comments.header = comment;
        }

        parts.push_back(code);
        for (char ch : code) {
            if (ch == '(')
                ++depth;
            else if (ch == ')')
                --depth;
            else if (ch == ';' && depth == 0) {
                end_i = j + 1;
                flat = "";
                for (size_t k = 0; k < parts.size(); ++k) {
                    if (k)
                        flat += ' ';
                    flat += parts[k];
                }
                return true;
            }
        }
        ++j;
    }
    return false;
}

static bool collect_instance(const std::vector<FormattedLine>& lines, size_t start, size_t& end_i,
                             std::string& flat, InstanceComments& comments) {
    std::vector<std::string> parts;
    int depth = 0;
    size_t j = start;
    while (j < lines.size()) {
        std::string stripped = lines[j].text;
        size_t sp = 0;
        while (sp < stripped.size() && (stripped[sp] == ' ' || stripped[sp] == '\t'))
            ++sp;
        stripped = stripped.substr(sp);

        std::string code = stripped;
        size_t line_comment = find_line_comment_start(stripped);
        if (line_comment != std::string::npos) {
            code = stripped.substr(0, line_comment);
            std::string comment = stripped.substr(line_comment);
            std::string port = last_named_port_before_comment(code);
            if (!port.empty())
                comments.port_comments.push_back({port, comment});
            else if (code.find('(') != std::string::npos && comments.header.empty())
                comments.header = comment;
        }

        parts.push_back(code);
        for (char ch : code) {
            if (ch == '(')
                ++depth;
            else if (ch == ')')
                --depth;
            else if (ch == ';' && depth == 0) {
                end_i = j + 1;
                flat = "";
                for (size_t k = 0; k < parts.size(); ++k) {
                    if (k)
                        flat += ' ';
                    flat += parts[k];
                }
                return true;
            }
        }
        ++j;
    }
    return false;
}

// Extract content of outermost (...) immediately before ;
static bool extract_port_list(const std::string& flat, std::string& port_list) {
    auto semi = flat.rfind(';');
    if (semi == std::string::npos)
        return false;
    int j = (int)semi - 1;
    while (j >= 0 && (flat[j] == ' ' || flat[j] == '\t'))
        --j;
    if (j < 0 || flat[j] != ')')
        return false;
    int close = j;
    int depth = 1;
    --j;
    while (j >= 0 && depth > 0) {
        if (flat[j] == ')')
            ++depth;
        else if (flat[j] == '(')
            --depth;
        --j;
    }
    port_list = flat.substr(j + 2, close - (j + 2));
    // trim
    size_t a = 0;
    while (a < port_list.size() &&
           (port_list[a] == ' ' || port_list[a] == '\t' || port_list[a] == '\n'))
        ++a;
    size_t b = port_list.size();
    while (b > a &&
           (port_list[b - 1] == ' ' || port_list[b - 1] == '\t' || port_list[b - 1] == '\n'))
        --b;
    port_list = port_list.substr(a, b - a);
    return true;
}

static std::string take_leading_portlist_block_comments(std::string& ports_str) {
    std::string comments;
    size_t i = 0;
    while (true) {
        size_t ws_start = i;
        while (i < ports_str.size() && (ports_str[i] == ' ' || ports_str[i] == '\t'))
            ++i;
        if (i + 1 >= ports_str.size() || ports_str[i] != '/' || ports_str[i + 1] != '*') {
            i = ws_start;
            break;
        }

        i += 2;
        while (i + 1 < ports_str.size() && !(ports_str[i] == '*' && ports_str[i + 1] == '/'))
            ++i;
        if (i + 1 >= ports_str.size()) {
            i = ws_start;
            break;
        }
        i += 2;
        comments += ports_str.substr(ws_start, i - ws_start);
    }

    if (!comments.empty())
        ports_str.erase(0, i);
    return comments;
}

static void append_comment(std::string& dst, const std::string& comment) {
    if (dst.empty())
        dst = comment;
    else
        dst += " " + comment;
}

static void remove_block_comments_from_instance_port_list(std::string& port_list,
                                                          InstanceComments& comments) {
    std::string code;
    bool in_string = false;
    bool escaped = false;
    for (size_t i = 0; i < port_list.size();) {
        char ch = port_list[i];
        if (in_string) {
            code += ch;
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                in_string = false;
            }
            ++i;
            continue;
        }
        if (ch == '"') {
            in_string = true;
            code += ch;
            ++i;
            continue;
        }
        if (i + 1 < port_list.size() && ch == '/' && port_list[i + 1] == '*') {
            size_t end = port_list.find("*/", i + 2);
            if (end == std::string::npos) {
                code += port_list.substr(i);
                break;
            }
            std::string comment = port_list.substr(i, end + 2 - i);
            std::string port = last_named_port_before_comment(code);
            if (!port.empty()) {
                if (!comments.port_comments.empty() && comments.port_comments.back().first == port)
                    append_comment(comments.port_comments.back().second, comment);
                else
                    comments.port_comments.push_back({port, comment});
            } else {
                append_comment(comments.header, comment);
            }
            i = end + 2;
            continue;
        }
        code += ch;
        ++i;
    }
    port_list = code;
}

// Parse named port connections .name(signal), ...
// Returns false if positional
static bool parse_named_ports(const std::string& port_list,
                              std::vector<std::pair<std::string, std::string>>& ports) {
    size_t i = 0, n = port_list.size();
    while (i < n) {
        while (i < n && (port_list[i] == ' ' || port_list[i] == '\t' || port_list[i] == '\n' ||
                         port_list[i] == ','))
            ++i;
        if (i >= n)
            break;
        if (port_list[i] != '.')
            return false; // positional
        ++i;
        size_t j = i;
        while (j < n && (std::isalnum((unsigned char)port_list[j]) || port_list[j] == '_'))
            ++j;
        std::string port_name = port_list.substr(i, j - i);
        i = j;
        while (i < n && (port_list[i] == ' ' || port_list[i] == '\t'))
            ++i;
        if (i >= n || port_list[i] != '(')
            return false;
        ++i;
        int depth = 1;
        size_t sig_start = i;
        while (i < n && depth > 0) {
            if (port_list[i] == '(')
                ++depth;
            else if (port_list[i] == ')')
                --depth;
            ++i;
        }
        std::string sig = port_list.substr(sig_start, i - 1 - sig_start);
        // trim sig
        size_t a = 0;
        while (a < sig.size() && (sig[a] == ' ' || sig[a] == '\t'))
            ++a;
        size_t b = sig.size();
        while (b > a && (sig[b - 1] == ' ' || sig[b - 1] == '\t'))
            --b;
        sig = sig.substr(a, b - a);
        ports.push_back({port_name, sig});
    }
    return !ports.empty();
}

// Split flat into (module_type, param_block, inst_name)
static bool split_inst_parts(const std::string& flat, std::string& module_type,
                             std::string& param_block, std::string& inst_name) {
    size_t i = 0, n = flat.size();
    // Skip leading whitespace
    while (i < n && (flat[i] == ' ' || flat[i] == '\t'))
        ++i;
    // Read module_type (word)
    size_t s = i;
    while (i < n && (std::isalnum((unsigned char)flat[i]) || flat[i] == '_'))
        ++i;
    if (i == s)
        return false;
    module_type = flat.substr(s, i - s);
    while (i < n && (flat[i] == ' ' || flat[i] == '\t'))
        ++i;

    param_block = "";
    if (i < n && flat[i] == '#') {
        auto ki = flat.find('(', i);
        if (ki == std::string::npos)
            return false;
        size_t k = ki + 1;
        int depth = 1;
        while (k < n && depth > 0) {
            if (flat[k] == '(')
                ++depth;
            else if (flat[k] == ')')
                --depth;
            ++k;
        }
        param_block = flat.substr(i, k - i);
        while (!param_block.empty() && (param_block.back() == ' ' || param_block.back() == '\t'))
            param_block.pop_back();
        i = k;
        while (i < n && (flat[i] == ' ' || flat[i] == '\t'))
            ++i;
    }

    // Read inst_name (word)
    s = i;
    while (i < n && (std::isalnum((unsigned char)flat[i]) || flat[i] == '_'))
        ++i;
    if (i == s)
        return false;
    inst_name = flat.substr(s, i - s);
    return true;
}

static std::vector<FormattedLine> expand_instances_pass(std::vector<FormattedLine> lines,
                                                        const FormatOptions& opts) {
    const std::string port_indent(opts.instance.port_indent_level * opts.indent_size, ' ');
    int m_before = opts.instance.instance_port_name_width;
    int m_inside = opts.instance.instance_port_between_paren_width;
    bool adaptive = opts.instance.align_adaptive;

    std::vector<FormattedLine> out;
    size_t i = 0;
    while (i < lines.size()) {
        const std::string& line = lines[i].text;

        size_t first = line.find_first_not_of(" \t");
        if (first == std::string::npos || !(std::isalpha((unsigned char)line[first]) ||
                                            line[first] == '_' || line[first] == '$')) {
            out.push_back(lines[i]);
            ++i;
            continue;
        }
        size_t word_end = first + 1;
        while (word_end < line.size() && (std::isalnum((unsigned char)line[word_end]) ||
                                          line[word_end] == '_' || line[word_end] == '$'))
            ++word_end;
        if (has(SV_KW, lower(line.substr(first, word_end - first)))) {
            out.push_back(lines[i]);
            ++i;
            continue;
        }
        if (line.find('(', word_end) == std::string::npos) {
            out.push_back(lines[i]);
            ++i;
            continue;
        }

        // Extract leading whitespace
        std::string indent;
        size_t ii = 0;
        while (ii < line.size() && (line[ii] == ' ' || line[ii] == '\t'))
            indent += line[ii++];

        // Collect the full instantiation (may span lines) and parse
        size_t end_i;
        std::string flat;
        InstanceComments comments;
        if (!collect_instance(lines, i, end_i, flat, comments)) {
            out.push_back(lines[i]);
            ++i;
            continue;
        }

        std::string module_type, param_block, inst_name;
        if (!split_inst_parts(flat, module_type, param_block, inst_name)) {
            for (size_t k = i; k < end_i; ++k)
                out.push_back(lines[k]);
            i = end_i;
            continue;
        }

        // Skip SV keywords
        if (has(SV_KW, lower(module_type)) || has(SV_KW, lower(inst_name))) {
            for (size_t k = i; k < end_i; ++k)
                out.push_back(lines[k]);
            i = end_i;
            continue;
        }

        std::string port_list;
        if (!extract_port_list(flat, port_list)) {
            for (size_t k = i; k < end_i; ++k)
                out.push_back(lines[k]);
            i = end_i;
            continue;
        }
        std::string leading_comments = take_leading_portlist_block_comments(port_list);
        remove_block_comments_from_instance_port_list(port_list, comments);
        std::vector<std::pair<std::string, std::string>> ports;
        if (!parse_named_ports(port_list, ports)) {
            for (size_t k = i; k < end_i; ++k)
                out.push_back(lines[k]);
            i = end_i;
            continue;
        }
        int max_port = 0, max_sig = 0;
        for (auto& [p, s] : ports) {
            max_port = std::max(max_port, (int)p.size());
            max_sig = std::max(max_sig, (int)s.size());
        }
        int eff_before = std::max(1, m_before - max_port - 1);
        int eff_inside = std::max(0, m_inside - max_sig);
        if (!adaptive && opts.tab_align && opts.indent_size > 1) {
            auto snap = [&](int pos) { return snap_to_indent_grid(pos, opts.indent_size); };
            int base = (int)indent.size() + (int)port_indent.size() + 1 + max_port;
            int open_paren = snap(base + eff_before);
            eff_before = open_paren - base;
            int close_paren = snap(open_paren + 1 + max_sig + eff_inside);
            eff_inside = close_paren - open_paren - 1 - max_sig;
        }

        std::string hdr = indent + module_type;
        if (!param_block.empty())
            hdr += " " + param_block;
        hdr += " " + inst_name + " (";
        if (!leading_comments.empty())
            hdr += " " + leading_comments;
        if (!comments.header.empty())
            hdr += " " + comments.header;
        out.push_back({hdr});
        size_t comment_index = 0;
        for (size_t k = 0; k < ports.size(); ++k) {
            auto& [port, sig] = ports[k];
            std::string comma = (k + 1 == ports.size()) ? "" : ",";
            std::string pline;
            if (adaptive) {
                int sb = std::max(1, m_before - (int)port.size() - 1);
                int si = std::max(0, m_inside - (int)sig.size());
                pline = indent + port_indent + "." + port + std::string(sb, ' ') + "(" + sig +
                        std::string(si, ' ') + ")" + comma;
            } else {
                std::string pname = port;
                pname.resize(std::max((int)pname.size(), max_port), ' ');
                std::string sname = sig;
                sname.resize(std::max((int)sname.size(), max_sig), ' ');
                pline = indent + port_indent + "." + pname + std::string(eff_before, ' ') + "(" +
                        sname + std::string(eff_inside, ' ') + ")" + comma;
            }
            while (!pline.empty() && pline.back() == ' ')
                pline.pop_back();
            if (comment_index < comments.port_comments.size() &&
                comments.port_comments[comment_index].first == port)
                pline += " " + comments.port_comments[comment_index++].second;
            out.push_back({pline});
        }
        out.push_back({indent + ");"});
        i = end_i;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Function/task call formatting pass
// ---------------------------------------------------------------------------

static std::string trim_copy(const std::string& s) {
    size_t a = 0;
    while (a < s.size() && (s[a] == ' ' || s[a] == '\t'))
        ++a;
    size_t b = s.size();
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t'))
        --b;
    return s.substr(a, b - a);
}

// ---------------------------------------------------------------------------
// Typedef enum and modport formatting passes
// ---------------------------------------------------------------------------

static bool is_ident_char(char c) { return std::isalnum((unsigned char)c) || c == '_' || c == '$'; }

static bool starts_with_word_at(const std::string& s, size_t pos, const std::string& word) {
    if (pos + word.size() > s.size() || lower(s.substr(pos, word.size())) != word)
        return false;
    if (pos > 0 && is_ident_char(s[pos - 1]))
        return false;
    return pos + word.size() == s.size() || !is_ident_char(s[pos + word.size()]);
}

static bool collect_statement_lines(const std::vector<std::string>& lines, size_t start,
                                    size_t& end_i, std::string& flat) {
    int paren = 0;
    int brace = 0;
    std::vector<std::string> parts;
    for (size_t i = start; i < lines.size(); ++i) {
        std::string trimmed = trim_copy(lines[i]);
        parts.push_back(trimmed);
        for (char ch : trimmed) {
            if (ch == '(')
                ++paren;
            else if (ch == ')' && paren > 0)
                --paren;
            else if (ch == '{')
                ++brace;
            else if (ch == '}' && brace > 0)
                --brace;
            else if (ch == ';' && paren == 0 && brace == 0) {
                end_i = i;
                flat.clear();
                for (size_t k = 0; k < parts.size(); ++k) {
                    if (k)
                        flat += ' ';
                    flat += parts[k];
                }
                return true;
            }
        }
    }
    return false;
}

static bool collect_statement_lines(const std::vector<FormattedLine>& lines, size_t start,
                                    size_t& end_i, std::string& flat) {
    int paren = 0;
    int brace = 0;
    std::vector<std::string> parts;
    for (size_t i = start; i < lines.size(); ++i) {
        std::string trimmed = trim_copy(lines[i].text);
        parts.push_back(trimmed);
        for (char ch : trimmed) {
            if (ch == '(')
                ++paren;
            else if (ch == ')' && paren > 0)
                --paren;
            else if (ch == '{')
                ++brace;
            else if (ch == '}' && brace > 0)
                --brace;
            else if (ch == ';' && paren == 0 && brace == 0) {
                end_i = i;
                flat.clear();
                for (size_t k = 0; k < parts.size(); ++k) {
                    if (k)
                        flat += ' ';
                    flat += parts[k];
                }
                return true;
            }
        }
    }
    return false;
}

struct EnumItemParsed {
    std::string name;
    std::string value;
    bool has_value{false};
};

static size_t find_top_level_equal(const std::string& text) {
    int paren = 0;
    int bracket = 0;
    int brace = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        char ch = text[i];
        if (ch == '(')
            ++paren;
        else if (ch == ')' && paren > 0)
            --paren;
        else if (ch == '[')
            ++bracket;
        else if (ch == ']' && bracket > 0)
            --bracket;
        else if (ch == '{')
            ++brace;
        else if (ch == '}' && brace > 0)
            --brace;
        else if (ch == '=' && paren == 0 && bracket == 0 && brace == 0)
            return i;
    }
    return std::string::npos;
}

static std::string pad_right(std::string s, int width) {
    if ((int)s.size() < width)
        s.resize(width, ' ');
    return s;
}

static std::vector<FormattedLine> format_enum_declaration_pass(std::vector<FormattedLine> lines,
                                                               const FormatOptions& opts) {
    const auto& eo = opts.enum_declaration;
    const std::string enum_indent(opts.indent_size, ' ');

    std::vector<FormattedLine> out;
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string& line = lines[i].text;
        size_t first = line.find_first_not_of(" \t");
        if (first == std::string::npos || !starts_with_word_at(line, first, "typedef")) {
            out.push_back(lines[i]);
            continue;
        }

        size_t end_i = i;
        std::string flat;
        if (!collect_statement_lines(lines, i, end_i, flat)) {
            out.push_back(lines[i]);
            continue;
        }

        std::string leading = line.substr(0, first);
        size_t typedef_pos = flat.find("typedef");
        size_t enum_pos = flat.find("enum", typedef_pos == std::string::npos ? 0 : typedef_pos + 7);
        size_t open = flat.find('{', enum_pos == std::string::npos ? 0 : enum_pos + 4);
        if (typedef_pos == std::string::npos || enum_pos == std::string::npos ||
            open == std::string::npos) {
            out.push_back(lines[i]);
            continue;
        }
        int depth = 1;
        size_t close = open + 1;
        for (; close < flat.size(); ++close) {
            if (flat[close] == '{')
                ++depth;
            else if (flat[close] == '}' && --depth == 0)
                break;
        }
        if (close >= flat.size()) {
            out.push_back(lines[i]);
            continue;
        }
        size_t semi = flat.find(';', close + 1);
        if (semi == std::string::npos) {
            out.push_back(lines[i]);
            continue;
        }

        std::string prefix = trim_copy(flat.substr(0, open));
        if (lower(prefix).find("typedef enum") == std::string::npos) {
            out.push_back(lines[i]);
            continue;
        }
        std::string body = flat.substr(open + 1, close - open - 1);
        std::string suffix = trim_copy(flat.substr(close + 1, semi - close - 1));
        auto raw_items = split_top_level(body);
        std::vector<EnumItemParsed> items;
        for (auto& item : raw_items) {
            std::string t = trim_copy(item);
            if (t.empty())
                continue;
            size_t eq = find_top_level_equal(t);
            if (eq == std::string::npos) {
                items.push_back({t, "", false});
            } else {
                items.push_back({trim_copy(t.substr(0, eq)), trim_copy(t.substr(eq + 1)), true});
            }
        }
        if (items.empty()) {
            for (size_t k = i; k <= end_i; ++k)
                out.push_back(lines[k]);
            i = end_i;
            continue;
        }

        int item_base_col = (int)leading.size() + opts.indent_size;
        int block_name_width = tab_aligned_width(eo.enum_name_min_width, opts);
        int block_value_width = tab_aligned_width(eo.enum_value_min_width, opts);
        if (eo.align && !eo.align_adaptive) {
            for (const auto& item : items) {
                block_name_width = std::max(block_name_width, (int)item.name.size() + 1);
                if (item.has_value)
                    block_value_width = std::max(block_value_width, (int)item.value.size());
            }
            if (opts.tab_align) {
                block_name_width =
                    snap_to_indent_grid(item_base_col + block_name_width, opts.indent_size) -
                    item_base_col;
                block_value_width =
                    snap_to_indent_grid(item_base_col + block_name_width + 2 + block_value_width,
                                        opts.indent_size) -
                    item_base_col - block_name_width - 2;
            }
        }

        out.push_back({leading + prefix + " {"});
        for (size_t k = 0; k < items.size(); ++k) {
            const auto& item = items[k];
            std::string comma = (k + 1 < items.size()) ? "," : "";
            std::string rendered;
            if (!eo.align) {
                rendered = item.name + (item.has_value ? " = " + item.value : "");
            } else {
                int name_width = block_name_width;
                int value_width = block_value_width;
                if (eo.align_adaptive) {
                    name_width = std::max(tab_aligned_width(eo.enum_name_min_width, opts),
                                          (int)item.name.size() + 1);
                    if (item.has_value)
                        value_width = std::max(tab_aligned_width(eo.enum_value_min_width, opts),
                                               (int)item.value.size());
                    if (opts.tab_align) {
                        name_width =
                            snap_to_indent_grid(item_base_col + name_width, opts.indent_size) -
                            item_base_col;
                        value_width =
                            snap_to_indent_grid(item_base_col + name_width + 2 + value_width,
                                                opts.indent_size) -
                            item_base_col - name_width - 2;
                    }
                }
                rendered = pad_right(item.name, name_width);
                if (item.has_value)
                    rendered += "= " + pad_right(item.value, value_width);
            }
            std::string enum_line = leading + enum_indent + rendered + comma;
            if (comma.empty()) {
                while (!enum_line.empty() && enum_line.back() == ' ')
                    enum_line.pop_back();
            }
            out.push_back({enum_line});
        }
        out.push_back({leading + "} " + suffix + ";"});
        i = end_i;
    }

    return out;
}

struct ModportItemParsed {
    std::string direction;
    std::string name;
};

struct ModportParsed {
    std::string name;
    std::vector<ModportItemParsed> items;
};

static bool parse_modport_statement(const std::string& flat, std::vector<ModportParsed>& modports) {
    size_t pos = 0;
    while (pos < flat.size() && std::isspace((unsigned char)flat[pos]))
        ++pos;
    if (!starts_with_word_at(flat, pos, "modport"))
        return false;
    pos += 7;
    size_t semi = flat.rfind(';');
    if (semi == std::string::npos)
        return false;
    std::string rest = flat.substr(pos, semi - pos);
    auto entries = split_top_level(rest);
    static const std::unordered_set<std::string> DIRS = {"input", "output", "inout",
                                                         "ref",   "import", "export"};

    for (auto& entry_raw : entries) {
        std::string entry = trim_copy(entry_raw);
        if (entry.empty())
            continue;
        size_t name_start = 0;
        while (name_start < entry.size() && std::isspace((unsigned char)entry[name_start]))
            ++name_start;
        size_t name_end = name_start;
        while (name_end < entry.size() && is_ident_char(entry[name_end]))
            ++name_end;
        if (name_end == name_start)
            return false;
        size_t open = entry.find('(', name_end);
        if (open == std::string::npos)
            return false;
        int depth = 1;
        size_t close = open + 1;
        for (; close < entry.size(); ++close) {
            if (entry[close] == '(')
                ++depth;
            else if (entry[close] == ')' && --depth == 0)
                break;
        }
        if (close >= entry.size())
            return false;

        ModportParsed mp;
        mp.name = entry.substr(name_start, name_end - name_start);
        auto items = split_top_level(entry.substr(open + 1, close - open - 1));
        for (auto& item_raw : items) {
            std::string item = trim_copy(item_raw);
            if (item.empty())
                continue;
            std::istringstream ss(item);
            std::string dir;
            ss >> dir;
            std::string remainder;
            std::getline(ss, remainder);
            remainder = trim_copy(remainder);
            if (dir.empty() || remainder.empty() || !has(DIRS, lower(dir)))
                return false;
            mp.items.push_back({dir, remainder});
        }
        if (mp.items.empty())
            return false;
        modports.push_back(std::move(mp));
    }
    return !modports.empty();
}

static std::vector<FormattedLine> format_modport_pass(std::vector<FormattedLine> lines,
                                                      const FormatOptions& opts) {
    const auto& mo = opts.modport;
    const std::string item_indent(opts.indent_size, ' ');

    std::vector<FormattedLine> out;
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string& line = lines[i].text;
        size_t first = line.find_first_not_of(" \t");
        if (first == std::string::npos || !starts_with_word_at(line, first, "modport")) {
            out.push_back(lines[i]);
            continue;
        }
        size_t end_i = i;
        std::string flat;
        if (!collect_statement_lines(lines, i, end_i, flat)) {
            out.push_back(lines[i]);
            continue;
        }
        std::vector<ModportParsed> modports;
        if (!parse_modport_statement(flat, modports)) {
            for (size_t k = i; k <= end_i; ++k)
                out.push_back(lines[k]);
            i = end_i;
            continue;
        }

        std::string leading = line.substr(0, first);
        for (size_t mi = 0; mi < modports.size(); ++mi) {
            const auto& mp = modports[mi];
            int item_base_col = (int)leading.size() + opts.indent_size;
            int block_dir_width = tab_aligned_width(mo.direction_min_width, opts);
            int block_signal_width = tab_aligned_width(mo.signal_min_width, opts);
            if (mo.align) {
                for (const auto& item : mp.items) {
                    block_dir_width = std::max(block_dir_width, (int)item.direction.size() + 1);
                    if (!mo.align_adaptive)
                        block_signal_width = std::max(block_signal_width, (int)item.name.size());
                }
                if (opts.tab_align) {
                    block_dir_width =
                        snap_to_indent_grid(item_base_col + block_dir_width, opts.indent_size) -
                        item_base_col;
                    block_signal_width =
                        snap_to_indent_grid(item_base_col + block_dir_width + block_signal_width,
                                            opts.indent_size) -
                        item_base_col - block_dir_width;
                }
            }
            out.push_back({leading + "modport " + mp.name + " ("});
            for (size_t pi = 0; pi < mp.items.size(); ++pi) {
                const auto& item = mp.items[pi];
                std::string comma = (pi + 1 < mp.items.size()) ? "," : "";
                std::string rendered;
                if (!mo.align) {
                    rendered = item.direction + " " + item.name;
                } else {
                    int dir_width = block_dir_width;
                    int signal_width = block_signal_width;
                    if (mo.align_adaptive) {
                        signal_width = std::max(tab_aligned_width(mo.signal_min_width, opts),
                                                (int)item.name.size());
                        if (opts.tab_align)
                            signal_width =
                                snap_to_indent_grid(item_base_col + dir_width + signal_width,
                                                    opts.indent_size) -
                                item_base_col - dir_width;
                    }
                    rendered =
                        pad_right(item.direction, dir_width) + pad_right(item.name, signal_width);
                }
                std::string port_line = leading + item_indent + rendered + comma;
                if (comma.empty()) {
                    while (!port_line.empty() && port_line.back() == ' ')
                        port_line.pop_back();
                }
                out.push_back({port_line});
            }
            out.push_back({leading + std::string(")") + (mi + 1 < modports.size() ? "," : ";")});
        }
        i = end_i;
    }

    return out;
}

static bool find_simple_call(const std::string& line, size_t& name_start, size_t& name_end,
                             size_t& open, size_t& close) {
    static const std::unordered_set<std::string> SKIP = {
        "if",    "for",      "foreach",  "while",       "repeat",   "wait", "case",
        "casex", "casez",    "module",   "macromodule", "function", "task", "covergroup",
        "class", "property", "sequence", "assert",      "assume",   "cover"};
    for (size_t i = 0; i < line.size(); ++i) {
        unsigned char ch = (unsigned char)line[i];
        if (!(std::isalpha(ch) || line[i] == '_' || line[i] == '$'))
            continue;
        size_t s = i++;
        while (i < line.size() &&
               (std::isalnum((unsigned char)line[i]) || line[i] == '_' || line[i] == '$'))
            ++i;
        size_t e = i;
        size_t j = i;
        while (j < line.size() && (line[j] == ' ' || line[j] == '\t'))
            ++j;
        if (j >= line.size() || line[j] != '(')
            continue;
        if (s > 0 && line[s - 1] == '.')
            continue;
        std::string name = line.substr(s, e - s);
        if (has(SKIP, lower(name)))
            continue;
        int depth = 1;
        size_t k = j + 1;
        for (; k < line.size(); ++k) {
            if (line[k] == '(')
                ++depth;
            else if (line[k] == ')' && --depth == 0)
                break;
        }
        if (k >= line.size())
            return false;
        name_start = s;
        name_end = e;
        open = j;
        close = k;
        return true;
    }
    return false;
}

static std::string render_call_single(const std::string& prefix, const std::string& name,
                                      const std::vector<std::string>& args,
                                      const std::string& suffix, const FunctionOptions& fo) {
    std::string r = prefix + name + (fo.space_before_paren ? " " : "") + "(";
    if (fo.space_inside_paren && !args.empty())
        r += " ";
    for (size_t i = 0; i < args.size(); ++i) {
        if (i)
            r += ", ";
        r += args[i];
    }
    if (fo.space_inside_paren && !args.empty())
        r += " ";
    r += ")" + suffix;
    return r;
}

static std::vector<FormattedLine> format_function_calls_pass(std::vector<FormattedLine> lines,
                                                                const FormatOptions& opts) {
    const auto& fo = opts.function;
    std::string text = render_lines(lines);
    auto disabled = find_disabled(text);

    std::vector<FormattedLine> out;
    int pos = 0;
    for (size_t li = 0; li < lines.size(); ++li) {
        const std::string& line = lines[li].text;
        int line_start = pos;
        pos += (int)line.size() + 1; // +1 for the newline consumed by getline
        if (line.find('\n') != std::string::npos) {
            out.push_back(lines[li]);
            continue;
        }
        if (in_disabled(line_start, disabled)) {
            out.push_back(lines[li]);
            continue;
        }
        size_t ns = 0, ne = 0, op = 0, cl = 0;
        if (!find_simple_call(line, ns, ne, op, cl)) {
            out.push_back(lines[li]);
            continue;
        }
        std::string args_text = line.substr(op + 1, cl - op - 1);
        auto raw_args = split_top_level(args_text);
        std::vector<std::string> args;
        for (auto& a : raw_args) {
            auto t = trim_copy(a);
            if (!t.empty())
                args.push_back(t);
        }

        std::string prefix = line.substr(0, ns);
        std::string name = line.substr(ns, ne - ns);
        std::string suffix = line.substr(cl + 1);
        std::string prefix_trimmed = trim_copy(prefix);
        std::string prefix_lower = lower(prefix_trimmed);
        auto is_bare_id = [](const std::string& s) -> bool {
            if (s.empty())
                return false;
            char c0 = s[0];
            if (!std::isalpha((unsigned char)c0) && c0 != '_' && c0 != '$')
                return false;
            for (size_t k = 1; k < s.size(); ++k) {
                char c = s[k];
                if (!std::isalnum((unsigned char)c) && c != '_' && c != '$')
                    return false;
            }
            return true;
        };
        if (is_bare_id(prefix_trimmed) || prefix_lower.find("function") != std::string::npos ||
            prefix_lower.find("task") != std::string::npos ||
            prefix_lower.find("module") != std::string::npos ||
            prefix_lower.find("class") != std::string::npos) {
            out.push_back(lines[li]);
            continue;
        }
        std::string single = render_call_single(prefix, name, args, suffix, fo);

        bool do_break = false;
        if (fo.break_policy == "always") {
            do_break = !args.empty();
        } else if (fo.break_policy == "auto") {
            do_break = ((int)single.size() > fo.line_length) ||
                       (fo.arg_count >= 0 && (int)args.size() >= fo.arg_count);
        }
        if (!do_break || fo.break_policy == "never") {
            out.push_back({single});
            continue;
        }

        std::string open_text = prefix + name + (fo.space_before_paren ? " " : "") + "(";
        if (fo.layout == "hanging") {
            std::string hang(open_text.size(), ' ');
            std::string r = open_text;
            for (size_t i = 0; i < args.size(); ++i) {
                if (i)
                    r += "\n" + hang;
                r += args[i];
                if (i + 1 < args.size())
                    r += ",";
            }
            r += ")" + suffix;
            out.push_back({r});
        } else {
            std::string base_indent(prefix.size(), ' ');
            std::string arg_indent = base_indent + std::string(std::max(0, opts.indent_size), ' ');
            std::string r = open_text + "\n";
            for (size_t i = 0; i < args.size(); ++i) {
                r += arg_indent + args[i];
                if (i + 1 < args.size())
                    r += ",";
                r += "\n";
            }
            r += base_indent + ")" + suffix;
            out.push_back({r});
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Module port-list formatting pass
// ---------------------------------------------------------------------------

// Given a single line that starts a module declaration,
// extract prefix (including '('), ports_str (between outermost parens), suffix (')...;').
// Returns false if not a single-line module header.
static bool extract_single_line_module_header(const std::string& line, std::string& prefix,
                                              std::string& ports_str, std::string& suffix_str) {
    size_t i = 0, n = line.size();
    // Skip leading whitespace
    while (i < n && (line[i] == ' ' || line[i] == '\t'))
        ++i;
    // Skip keyword (module/macromodule)
    size_t kw_start = i;
    while (i < n && std::isalnum((unsigned char)line[i]))
        ++i;
    std::string keyword = lower(line.substr(kw_start, i - kw_start));
    if (keyword != "module" && keyword != "macromodule")
        return false;
    while (i < n && (line[i] == ' ' || line[i] == '\t'))
        ++i;
    // Skip module name (identifier)
    while (i < n && (std::isalnum((unsigned char)line[i]) || line[i] == '_'))
        ++i;
    while (i < n && (line[i] == ' ' || line[i] == '\t'))
        ++i;
    // Skip optional #(...) param block
    if (i < n && line[i] == '#') {
        ++i;
        while (i < n && (line[i] == ' ' || line[i] == '\t'))
            ++i;
        if (i < n && line[i] == '(') {
            int depth = 1;
            ++i;
            while (i < n && depth > 0) {
                if (line[i] == '(')
                    ++depth;
                else if (line[i] == ')')
                    --depth;
                ++i;
            }
            while (i < n && (line[i] == ' ' || line[i] == '\t'))
                ++i;
        }
    }
    // Expect '('
    if (i >= n || line[i] != '(')
        return false;
    size_t open_paren = i;
    ++i;
    // Find matching ')' on same line
    int depth = 1;
    while (i < n && depth > 0) {
        if (line[i] == '(')
            ++depth;
        else if (line[i] == ')') {
            if (--depth == 0)
                break;
        }
        if (depth > 0)
            ++i;
    }
    if (i >= n || depth != 0)
        return false;
    size_t close_paren = i;
    ++i;
    // Skip whitespace, expect ';'
    while (i < n && (line[i] == ' ' || line[i] == '\t'))
        ++i;
    if (i >= n || line[i] != ';')
        return false;
    size_t semi_pos = i;
    ++i;
    // Must be end of line
    while (i < n && (line[i] == ' ' || line[i] == '\t'))
        ++i;
    if (i != n)
        return false;

    prefix = line.substr(0, open_paren + 1); // up to and including '('
    ports_str = line.substr(open_paren + 1, close_paren - open_paren - 1);
    suffix_str = line.substr(close_paren, semi_pos - close_paren + 1); // ')...;'
    return true;
}

static std::vector<FormattedLine> format_portlist_pass(std::vector<FormattedLine> lines,
                                                       const FormatOptions& opts) {
    const std::string indent_unit(opts.indent_size, ' ');

    std::vector<FormattedLine> out;
    auto push_original_lines = [&](size_t first, size_t last) {
        for (size_t k = first; k <= last; ++k)
            out.push_back(lines[k]);
    };
    for (size_t i = 0; i < lines.size(); ++i) {
        std::string line = lines[i].text;

        size_t first = line.find_first_not_of(" \t");
        bool module_start =
            first != std::string::npos && (line.compare(first, 7, "module ") == 0 ||
                                           line.compare(first, 12, "macromodule ") == 0);
        size_t consumed_end = i;
        if (module_start && line.find(';') == std::string::npos) {
            std::string flat = line;
            size_t j = i + 1;
            for (; j < lines.size(); ++j) {
                std::string trimmed = trim_copy(lines[j].text);
                flat += " " + trimmed;
                if (trimmed.find(");") != std::string::npos)
                    break;
            }
            if (j < lines.size()) {
                line = flat;
                consumed_end = j;
            }
        }

        // Try to extract single-line module header: module foo [#(...)] (ports);
        std::string prefix, ports_str, suffix_str;
        if (!extract_single_line_module_header(line, prefix, ports_str, suffix_str)) {
            push_original_lines(i, consumed_end);
            i = consumed_end;
            continue;
        }
        prefix += take_leading_portlist_block_comments(ports_str);

        auto ports = split_top_level(ports_str);
        std::vector<std::string> trimmed_ports;
        for (auto& p : ports) {
            size_t a = 0;
            while (a < p.size() && (p[a] == ' ' || p[a] == '\t'))
                ++a;
            size_t b = p.size();
            while (b > a && (p[b - 1] == ' ' || p[b - 1] == '\t'))
                --b;
            if (b > a)
                trimmed_ports.push_back(p.substr(a, b - a));
        }
        if (trimmed_ports.empty()) {
            push_original_lines(i, consumed_end);
            i = consumed_end;
            continue;
        }

        // Leading whitespace
        std::string leading_ws;
        for (size_t k = 0; k < line.size() && (line[k] == ' ' || line[k] == '\t'); ++k)
            leading_ws += line[k];
        std::string port_indent = leading_ws + indent_unit;

        // ANSI vs non-ANSI detection
        static const std::unordered_set<std::string> ANSI_DIR = {"input", "output", "inout", "ref"};
        bool is_ansi = false;
        for (auto& p : trimmed_ports) {
            std::istringstream ss(p);
            std::string first;
            ss >> first;
            if (!first.empty() && has(ANSI_DIR, lower(first))) {
                is_ansi = true;
                break;
            }
        }

        std::string new_lines_str;
        if (is_ansi) {
            std::string port_lines;
            for (size_t k = 0; k < trimmed_ports.size(); ++k) {
                std::string comma =
                    (k + 1 < trimmed_ports.size() || opts.port_declaration.align) ? "," : "";
                port_lines += port_indent + trimmed_ports[k] + comma + "\n";
            }
            if (!port_lines.empty() && port_lines.back() == '\n')
                port_lines.pop_back();
            if (opts.port_declaration.align) {
                auto port_flines = align_port_pass(text_to_lines(port_lines), opts);
                port_lines = render_lines(port_flines);
                std::vector<std::string> aligned_lines;
                {
                    std::istringstream ss(port_lines);
                    std::string l;
                    while (std::getline(ss, l)) {
                        size_t last = l.find_last_not_of(" \t");
                        if (last != std::string::npos && l[last] == ',') {
                            size_t before = l.find_last_not_of(" \t", last - 1);
                            l.erase(before + 1, last - before - 1);
                        }
                        aligned_lines.push_back(l);
                    }
                }
                port_lines.clear();
                for (size_t ai = 0; ai < aligned_lines.size(); ++ai) {
                    if (ai)
                        port_lines += '\n';
                    port_lines += aligned_lines[ai];
                }
                size_t last = port_lines.find_last_not_of(" \t\n");
                if (last != std::string::npos && port_lines[last] == ',')
                    port_lines.erase(last, 1);
                while (!port_lines.empty() &&
                       (port_lines.back() == ' ' || port_lines.back() == '\t'))
                    port_lines.pop_back();
            }
            new_lines_str = prefix + "\n" + port_lines + "\n" + leading_ws + suffix_str;
        } else {
            // Check all are simple identifiers (char scan)
            bool all_simple = true;
            for (auto& p : trimmed_ports) {
                if (p.empty()) {
                    all_simple = false;
                    break;
                }
                char c0 = p[0];
                if (!std::isalpha((unsigned char)c0) && c0 != '_' && c0 != '$') {
                    all_simple = false;
                    break;
                }
                for (size_t k = 1; k < p.size(); ++k) {
                    char c = p[k];
                    if (!std::isalnum((unsigned char)c) && c != '_' && c != '$') {
                        all_simple = false;
                        break;
                    }
                }
                if (!all_simple)
                    break;
            }
            if (!all_simple) {
                push_original_lines(i, consumed_end);
                i = consumed_end;
                continue;
            }

            std::string port_block;
            if (opts.port.non_ansi_port_per_line_enabled && opts.port.non_ansi_port_per_line > 0) {
                int n = opts.port.non_ansi_port_per_line;
                if ((int)trimmed_ports.size() <= n) {
                    push_original_lines(i, consumed_end);
                    i = consumed_end;
                    continue;
                }
                for (size_t gi = 0; gi < trimmed_ports.size(); gi += (size_t)n) {
                    size_t end_g = std::min(gi + (size_t)n, trimmed_ports.size());
                    std::string comma = (end_g < trimmed_ports.size()) ? "," : "";
                    std::string grp_line = port_indent;
                    for (size_t k = gi; k < end_g; ++k) {
                        if (k > gi)
                            grp_line += ", ";
                        grp_line += trimmed_ports[k];
                    }
                    grp_line += comma;
                    port_block += grp_line + "\n";
                }
            } else if (opts.port.non_ansi_port_max_line_length_enabled &&
                       opts.port.non_ansi_port_max_line_length > 0) {
                int max_len = opts.port.non_ansi_port_max_line_length;
                std::vector<std::string> current;
                for (size_t pi = 0; pi < trimmed_ports.size(); ++pi) {
                    std::string candidate = port_indent;
                    for (size_t k = 0; k < current.size(); ++k) {
                        if (k)
                            candidate += ", ";
                        candidate += current[k];
                    }
                    candidate += ", " + trimmed_ports[pi];
                    if (!current.empty() && (int)candidate.size() > max_len) {
                        std::string row = port_indent;
                        for (size_t k = 0; k < current.size(); ++k) {
                            if (k)
                                row += ", ";
                            row += current[k];
                        }
                        row += ",";
                        port_block += row + "\n";
                        current = {trimmed_ports[pi]};
                    } else {
                        current.push_back(trimmed_ports[pi]);
                    }
                }
                if (!current.empty()) {
                    std::string row = port_indent;
                    for (size_t k = 0; k < current.size(); ++k) {
                        if (k)
                            row += ", ";
                        row += current[k];
                    }
                    port_block += row + "\n";
                }
            } else {
                for (size_t k = 0; k < trimmed_ports.size(); ++k) {
                    std::string comma = (k + 1 < trimmed_ports.size()) ? "," : "";
                    port_block += port_indent + trimmed_ports[k] + comma + "\n";
                }
            }
            if (!port_block.empty() && port_block.back() == '\n')
                port_block.pop_back();
            new_lines_str = prefix + "\n" + port_block + "\n" + leading_ws + suffix_str;
        }

        // Split new_lines_str by \n and push each line
        auto new_flines = text_to_lines(new_lines_str);
        for (auto& fl : new_flines)
            out.push_back(std::move(fl));
        i = consumed_end;
    }

    return out;
}

// ---------------------------------------------------------------------------
// Formatter pipeline phases
// ---------------------------------------------------------------------------
//
// The formatter is organized as:
//   Phase 1: token-level spacing, line breaks, and indentation   (token loop)
//   Phase 2: structural layout / reflow of multiline constructs  (post-token)
//   Phase 3: line-group alignment                                (post-token)
//
// All post-token passes operate on vectors of FormattedLine.  Each line
// carries its syntactic kind (from the slang LineClassifier), so passes
// can identify line roles without re-parsing text or maintaining fragile
// line-number maps.
//
// All passes now operate natively on FormattedLine vectors.

static std::vector<FormattedLine> run_module_header_layout_phase(std::vector<FormattedLine> lines,
                                                                 const FormatOptions& opts) {
    return format_portlist_pass(std::move(lines), opts);
}

static std::vector<FormattedLine> run_line_group_alignment_phase(std::vector<FormattedLine> lines,
                                                                 const FormatOptions& opts) {
    if (opts.statement.align)
        lines = align_assign_pass(std::move(lines), opts);
    if (opts.var_declaration.align)
        lines = align_var_pass(std::move(lines), opts);
    if (opts.port_declaration.align)
        lines = align_port_pass(std::move(lines), opts);
    return lines;
}

static std::vector<FormattedLine> run_structural_layout_phase(
    std::vector<FormattedLine> lines, const FormatOptions& opts) {
    lines = format_enum_declaration_pass(std::move(lines), opts);
    lines = format_modport_pass(std::move(lines), opts);
    if (opts.instance.align)
        lines = expand_instances_pass(std::move(lines), opts);
    lines = format_function_calls_pass(std::move(lines), opts);
    return lines;
}

static std::vector<FormattedLine> run_post_token_pipeline(std::vector<FormattedLine> lines,
                                                          const FormatOptions& opts) {
    lines = run_module_header_layout_phase(std::move(lines), opts);
    lines = run_line_group_alignment_phase(std::move(lines), opts);
    lines = run_structural_layout_phase(std::move(lines), opts);
    return lines;
}

static void verify_safe_mode_unchanged(const std::string& source, const std::string& formatted,
                                       const FormatOptions& opts) {
    if (!opts.safe_mode)
        return;

    auto strip = [](const std::string& s) {
        std::string r;
        r.reserve(s.size());
        for (char c : s)
            if (!std::isspace((unsigned char)c))
                r += c;
        return r;
    };
    if (strip(source) != strip(formatted))
        throw SafeModeError(
            "Formatter safe-mode: non-whitespace content changed — formatting aborted");
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// format_source — main entry point
// ---------------------------------------------------------------------------

std::string format_source(const std::string& source, const FormatOptions& opts,
                          std::shared_ptr<const SyntaxTree> ext_tree) {
    // -----------------------------------------------------------------------
    // STEP 1: Build (or reuse) the slang SyntaxTree
    //
    // The SyntaxTree is slang's parse tree — it represents the full structure
    // of the Verilog/SystemVerilog source (modules, ports, statements, etc.)
    // without doing any semantic elaboration.
    //
    // If the caller already parsed the same source (ext_tree != null), reuse
    // it to avoid a redundant parse.  Otherwise we own our own SourceManager.
    //
    // source_only_tree = true means we are using the caller's tree; the token
    // positions in that tree map directly to `source` bytes.
    // -----------------------------------------------------------------------
    const std::string& input = source;
    std::unique_ptr<SourceManager> sm_owned;
    std::shared_ptr<const SyntaxTree> tree;
    bool source_only_tree = false;
    if (ext_tree) {
        tree = ext_tree;
        source_only_tree = true;
    } else {
        sm_owned = std::make_unique<SourceManager>();
        tree = SyntaxTree::fromText(input, *sm_owned);
    }

    // -----------------------------------------------------------------------
    // STEP 3: Build helper data structures
    //
    // indent_unit  — the string for one indent level, e.g. "    " (4 spaces).
    //
    // disabled     — list of [start, end) byte ranges that are inside
    //                `// verilog_format: off` … `// verilog_format: on`
    //                regions.  Tokens inside these ranges are copied verbatim.
    //
    // tokens       — ordered list of Tok structs extracted from the CST.
    //                Each Tok has: text, pos (byte offset), and bool flags
    //                (whitespace, comment, directive, keyword, etc.).
    // -----------------------------------------------------------------------
    const std::string indent_unit(opts.indent_size, ' ');
    auto disabled = find_disabled(input);
    auto tokens = collect_cst_tokens(input, *tree, source_only_tree);

    // -----------------------------------------------------------------------
    // STEP 4: Classify every original source line by its syntactic role
    //
    // LineClassifier walks the SyntaxTree and assigns each line a LineKind
    // (e.g. port declaration, variable declaration, assignment, instance, …).
    // This is used later by the alignment passes so they only align lines of
    // the same kind within a contiguous block.
    // -----------------------------------------------------------------------
    std::unordered_map<int, LineKind> orig_line_kinds;
    {
        LineClassifier clf(tree->sourceManager());
        clf.visit(tree->root());
        orig_line_kinds = std::move(clf.kinds);
    }

    // -----------------------------------------------------------------------
    // STEP 5: Build byte-offset → original line number table
    //
    // pos_to_orig_line[byte_offset] = 0-based line number in `input`.
    // Used to detect whether two tokens are on the same original line (needed
    // for the inline-comment logic below).
    // -----------------------------------------------------------------------
    std::vector<int> pos_to_orig_line;
    pos_to_orig_line.reserve(input.size() + 1);
    {
        int ln = 0;
        for (char c : input) {
            pos_to_orig_line.push_back(ln);
            if (c == '\n')
                ++ln;
        }
        pos_to_orig_line.push_back(ln); // sentinel for the byte past the last char
    }

    // -----------------------------------------------------------------------
    // STEP 6: State variables for the token-walking loop
    //
    // The main loop below walks every token exactly once and builds `out` —
    // the formatted output string.  All the variables below track the
    // "current state of the world" as we advance through the token stream.
    // -----------------------------------------------------------------------

    // fmt_line_count  — how many '\n' characters have been written to `out`
    //                   so far.  Used to assign LineKind to formatted lines.
    int fmt_line_count = 0;
    std::unordered_map<int, LineKind> fmt_line_kinds;
    std::unordered_set<int> fmt_line_seen; // track first token per orig line

    std::string out;
    out.reserve(input.size() + input.size() / 4); // pre-allocate ~25% extra

    // indent_level — current nesting depth (in units of indent_unit).
    // indent_stack — stack of how much each INDENT_OPEN keyword added, so
    //               the matching INDENT_CLOSE can pop the exact same amount.
    //               (outmost module/interface/package blocks add
    //                opts.default_indent_level_inside_outmost_block, everything else adds 1.)
    int indent_level = 0;
    std::vector<int> indent_stack;

    // at_bol    — true when the next character we emit starts a fresh line,
    //             so we must emit the indentation prefix first.
    bool at_bol = true;

    // dim_depth — how many unmatched '[' we have seen since the last ';'.
    //             Nonzero means we are inside an array dimension like [7:0].
    //             Spacing rules differ inside dimensions.
    int dim_depth = 0;

    // paren_depth — how many unmatched '(' we have seen.
    //               Used to distinguish top-level ';' (statement end) from
    //               ';' inside a for(;;) header.
    int paren_depth = 0;
    std::vector<ParenKind> paren_stack;
    std::vector<bool> for_header_stack;

    // do_depth  — counts nested `do` keywords so we know when `end while`
    //             belongs to a do…while (MustAppend) vs. a plain `while`.
    int do_depth = 0;

    // pending_nl — true means we want a newline BEFORE the next token, but
    //              haven't emitted it yet.  Deferred so that inline comments
    //              can suppress it if they live on the same source line.
    bool pending_nl = false;

    // blank_pend — number of extra blank lines to insert before the next
    //              token (capped at opts.blank_lines_between_items).
    int blank_pend = 0;

    // in_pp_cond — true while we are inside a preprocessor conditional that
    //              takes an argument on the same line, e.g. `ifdef MACRO_NAME.
    //              We need a newline after the argument.
    bool in_pp_cond = false;

    // after_dis  — true immediately after we exit a disabled region.
    //              If the next whitespace token contains a newline, we must
    //              emit pending_nl so the re-enabled code starts on its own line.
    bool after_dis = false;

    // struct_pend — true after we see `struct` or `union` keyword; the next
    //               `{` should be treated as a struct-brace (indented block)
    //               rather than a concatenation brace.
    bool struct_pend = false;

    // case_expr_pending / case_expr_depth
    //   After `case`/`casex`/`casez`/`caseinside` we expect a `(expr)`.
    //   pending goes true at the keyword; depth records the paren nesting level
    //   of the opening `(`.  When we see the matching `)`, we schedule a newline
    //   (the case items start on the next line).
    bool case_expr_pending = false;
    int case_expr_depth = -1;

    // control_expr_pending / control_expr_depth
    //   Same idea for `if`/`for`/`foreach`/`while`/`repeat` — after the closing
    //   `)` of the condition, we set single_stmt_pending so the body gets
    //   an extra indent level if it has no `begin`.
    bool control_expr_pending = false;
    int control_expr_depth = -1;

    // single_stmt_pending — set after the `)` of an if/for/while condition.
    //                       If the very next token at BOL is NOT `begin`, we
    //                       bump indent_level by 1 (single_stmt_active = true).
    // single_stmt_active  — true while we are inside a bodyless single-statement
    //                       block; reset (and indent_level decremented) at `;`.
    bool single_stmt_pending = false;
    bool single_stmt_active = false;

    // do_while_tail — true for the `while` token that closes a `do…while`.
    //                 Prevents that `while` from being treated as a loop header
    //                 (which would set control_expr_pending again).
    bool do_while_tail = false;

    // brace_stk — stack that records whether each open `{` is a struct/union
    //             brace (value "struct") or something else ("other").
    //             Only struct braces trigger indentation.
    std::vector<std::string> brace_stk;

    // prev — pointer to the previous non-whitespace Tok, used by spaces_req()
    //        and break_dec() to decide spacing/line-break between token pairs.
    const Tok* prev = nullptr;
    bool prev_at_procedural = false;

    // -----------------------------------------------------------------------
    // Helper lambdas
    //
    // flush_nl() — actually writes the deferred newlines to `out`.
    //              Called just before emitting a token (unless MustAppend).
    //
    // emit(text) — writes `text` to `out`, prepending indentation if we are
    //              at the beginning of a line (at_bol == true).
    // -----------------------------------------------------------------------
    auto flush_nl = [&]() {
        if (pending_nl) {
            out += '\n';
            at_bol = true;
            pending_nl = false;
            ++fmt_line_count;
        }
        if (blank_pend > 0) {
            if (!at_bol) {
                out += '\n';
                at_bol = true;
                ++fmt_line_count;
            }
            for (int k = 0; k < blank_pend; ++k) {
                out += '\n';
                at_bol = true;
                ++fmt_line_count;
            }
            blank_pend = 0;
        }
    };
    auto emit = [&](const std::string& text) {
        if (at_bol) {
            for (int k = 0; k < indent_level; ++k)
                out += indent_unit;
            at_bol = false;
        }
        out += text;
    };

    // -----------------------------------------------------------------------
    // STEP 7: Main token loop — single pass over all tokens
    //
    // For each token we:
    //   A. Skip / passthrough disabled-region tokens verbatim.
    //   B. Skip whitespace tokens (we reconstruct spacing from scratch).
    //   C. Decide spacing (spaces_req) and line-break (break_dec) from the
    //      previous token.
    //   D. Handle inline comments (don't let pending_nl split them off).
    //   E. Flush pending newlines or append-space depending on break decision.
    //   F. Handle single-statement indent (no `begin` after if/for/while).
    //   G. Decrement indent BEFORE emitting INDENT_CLOSE keywords (end, endmodule, …)
    //      so the closing keyword aligns with the matching open keyword.
    //   H. Emit the token text (with keyword-case transformation if needed).
    //   I. Record orig→fmt line mapping.
    //   J. Update bracket/paren/dim depth counters.
    //   K. Post-emit: open new indent level, schedule newlines, track pp-conds.
    // -----------------------------------------------------------------------
    for (size_t i = 0; i < tokens.size(); ++i) {
        const Tok& tok = tokens[i];

        // --- A. Disabled region — pass through verbatim ---
        // Tokens between `// verilog_format: off` and `// verilog_format: on`
        // are copied to `out` exactly as-is, with no reformatting at all.
        // We still flush any pending newline first so the disabled block starts
        // on a fresh line.  `prev` is NOT updated so the token before the
        // disabled region doesn't influence spacing after it.
        if (in_disabled(tok.pos, disabled)) {
            flush_nl();
            for (char c : tok.text)
                if (c == '\n')
                    ++fmt_line_count;
            out += tok.text;
            at_bol = !tok.text.empty() && tok.text.back() == '\n';
            after_dis = !at_bol; // remember we just left a disabled region mid-line
            continue;            // don't update prev
        }

        // --- B. Whitespace tokens — discard, but extract blank-line info ---
        // We never copy original whitespace; spacing is completely rewritten.
        // However, we do respect blank lines between items up to the configured
        // maximum (opts.blank_lines_between_items).
        if (tok.whitespace) {
            int nl = (int)std::count(tok.text.begin(), tok.text.end(), '\n');
            // If a disabled region ended mid-line and now we see a newline in
            // the following whitespace, emit the pending newline.
            if (after_dis && nl >= 1)
                pending_nl = true;
            after_dis = false;
            if (nl > 1) {
                // More than one newline = at least one blank line between items.
                // extra = number of blank lines (nl-1 because first \n ends the
                // current line, subsequent \n's are the blank lines themselves).
                int extra = std::min(nl - 1, opts.blank_lines_between_items);
                blank_pend = std::max(blank_pend, extra);
            }
            continue;
        }

        // --- C. Decide spacing and line-break between prev and tok ---
        // in_dim: inside array dimension brackets [hi:lo], spacing is tighter.
        // spaces: how many spaces to insert between prev and tok (0 or 1).
        // dec:    SD::MustWrap   → force a newline before tok
        //         SD::MustAppend → force tok onto the same line as prev
        //         SD::Undecided  → use pending_nl / blank_pend to decide
        bool in_dim = dim_depth > 0;
        bool in_for_header =
            std::any_of(for_header_stack.begin(), for_header_stack.end(), [](bool v) { return v; });
        bool procedural_at = prev && ((tok.text == "@" && is_procedural_event_keyword(*prev)) ||
                                      (prev->text == "@" && prev_at_procedural));
        ParenKind paren_kind = ParenKind::Ordinary;
        if (prev && (prev->text == "(" || tok.text == ")")) {
            if (!paren_stack.empty())
                paren_kind = paren_stack.back();
        } else if (prev && tok.text == "(")
            paren_kind = classify_paren(*prev, procedural_at);
        int spaces = 0;
        SD dec = SD::Undecided;
        if (prev) {
            spaces = spaces_req(*prev, tok, opts, in_dim, in_for_header, !paren_stack.empty(),
                                paren_kind, procedural_at);
            dec = break_dec(*prev, tok, opts, in_dim);
        }

        // --- D. Inline-comment suppression ---
        // A `;` sets pending_nl=true (statement ended → new line).
        // But if the very next token is a comment that lives on the same
        // original source line (e.g. `foo; // comment`), we must NOT emit
        // the newline yet — the comment must stay on the same line as `foo;`.
        bool inline_comment = false;
        if ((tok.comment) && prev) {
            int prev_ln =
                (prev->pos < (int)pos_to_orig_line.size()) ? pos_to_orig_line[prev->pos] : 0;
            int tok_ln = (tok.pos < (int)pos_to_orig_line.size()) ? pos_to_orig_line[tok.pos] : 0;
            inline_comment = prev_ln == tok_ln; // same original line → inline
        }

        // --- Special: `end while` in a do…while statement ---
        // In `do begin … end while (cond);` the `while` after `end` is NOT
        // the start of a new while-loop — it is the tail of the do…while.
        // We must append it to the same line as `end` (MustAppend) and must
        // NOT treat it as a new control-expression header.
        do_while_tail = false;
        if (prev && prev->lo == "end" && is_keyword(tok) && tok.lo == "while") {
            if (do_depth > 0) {
                dec = SD::MustAppend;
                --do_depth;
                do_while_tail = true;
            } else {
                dec = SD::Undecided;
            }
        }

        // Suppress the pending newline if this comment is on the same source line.
        if (inline_comment && pending_nl)
            pending_nl = false;

        // --- E. Emit newline / spacing based on break decision ---
        if (dec == SD::MustWrap) {
            // Force newline before this token regardless of pending_nl.
            pending_nl = false;
            if (!at_bol) {
                out += '\n';
                at_bol = true;
                ++fmt_line_count;
            }
            for (int k = 0; k < blank_pend; ++k) {
                out += '\n';
                ++fmt_line_count;
            }
            blank_pend = 0;
        } else if (dec == SD::MustAppend) {
            // Force this token onto the same line — cancel any pending newline.
            if (pending_nl) {
                pending_nl = false;
                blank_pend = 0;
            }
            if (!at_bol && spaces > 0)
                out += std::string(spaces, ' ');
        } else {
            // Normal: flush deferred newlines first, then add spaces.
            flush_nl();
            if (!at_bol && spaces > 0)
                out += std::string(spaces, ' ');
        }

        // --- F. Single-statement indent (bodyless if/for/while body) ---
        // single_stmt_pending is set after the `)` closing an if/for/while
        // condition.  We wait until we are truly at the start of a new line
        // (at_bol) to decide:
        //   • If the next token is `begin` → no extra indent needed (begin
        //     will open its own indented block via INDENT_OPEN).
        //   • Otherwise → bump indent_level by 1 for this single statement.
        if (single_stmt_pending && at_bol) {
            if (is_keyword(tok) && tok.lo == "begin") {
                single_stmt_pending = false; // begin handles its own indent
            } else {
                ++indent_level;
                single_stmt_pending = false;
                single_stmt_active = true; // remember to undo at the next `;`
            }
        }

        // --- G. Indent-close: decrement indent BEFORE emitting the token ---
        // Keywords like `end`, `endmodule`, `endfunction`, `join`, etc. are
        // in INDENT_CLOSE.  They should appear at the SAME indentation level
        // as the matching open keyword, so we reduce indent_level first.
        //
        // Closing `}` of a struct/union block works the same way — the `}`
        // should align with the `struct {` line.
        if (is_keyword(tok) && has(INDENT_CLOSE, tok.lo)) {
            int delta = indent_stack.empty() ? 1 : indent_stack.back();
            if (!indent_stack.empty())
                indent_stack.pop_back();
            indent_level = std::max(0, indent_level - delta);
        } else if (is_close_group(tok) && tok.text == "}" && !brace_stk.empty() &&
                   brace_stk.back() == "struct") {
            int delta = indent_stack.empty() ? 1 : indent_stack.back();
            if (!indent_stack.empty())
                indent_stack.pop_back();
            indent_level = std::max(0, indent_level - delta);
        }

        // --- H. Emit the token ---
        // Keywords get their case transformed (UPPERCASE / lowercase / as-is)
        // according to opts.keyword_case.  All other tokens are emitted verbatim.
        // emit() prepends the indentation string if at_bol is true.
        if (is_keyword(tok))
            emit(kw_case(tok.text, opts.keyword_case));
        else
            emit(tok.text);

        // --- I. Assign LineKind to formatted line ---
        // Only the first token from each original line is recorded.
        // Directly maps orig_line_kinds to the current formatted line.
        if (!in_disabled(tok.pos, disabled) && !tok.whitespace) {
            int orig_ln = (tok.pos < (int)pos_to_orig_line.size()) ? pos_to_orig_line[tok.pos] : 0;
            if (fmt_line_seen.insert(orig_ln).second) {
                auto kit = orig_line_kinds.find(orig_ln);
                if (kit != orig_line_kinds.end())
                    fmt_line_kinds.emplace(fmt_line_count, kit->second);
            }
        }

        // --- J. Update bracket/paren/dim depth counters ---
        // dim_depth tracks `[…]` nesting for array dimensions.
        // paren_depth tracks `(…)` nesting.
        // When we see `(` after case/if/for/while, record the depth so we
        // know which `)` closes the expression.
        // `;` resets dim_depth because dimensions never span statements.
        if (tok.text == "[")
            ++dim_depth;
        else if (tok.text == "]" && dim_depth > 0)
            --dim_depth;
        else if (tok.text == "(") {
            ++paren_depth;
            paren_stack.push_back(prev ? classify_paren(*prev, prev_at_procedural)
                                       : ParenKind::Ordinary);
            for_header_stack.push_back(prev && is_keyword(*prev) &&
                                       (prev->lo == "for" || prev->lo == "foreach"));
            if (case_expr_pending) {
                case_expr_depth = paren_depth; // remember which ')' ends the case expr
                case_expr_pending = false;
            }
            if (control_expr_pending) {
                control_expr_depth = paren_depth; // remember which ')' ends the control expr
                control_expr_pending = false;
            }
        } else if (tok.text == ")" && paren_depth > 0) {
            if (case_expr_depth == paren_depth) {
                // Closing `)` of `case (expr)` → next token goes on a new line
                pending_nl = true;
                case_expr_depth = -1;
            }
            if (control_expr_depth == paren_depth) {
                // Closing `)` of `if (cond)` / `for (…)` / etc.
                // → schedule a newline so the body starts on a fresh line.
                // `if (cond) begin` suppresses this via MustAppend in break_dec.
                single_stmt_pending = true;
                pending_nl = true;
                control_expr_depth = -1;
            }
            --paren_depth;
            if (!paren_stack.empty())
                paren_stack.pop_back();
            if (!for_header_stack.empty())
                for_header_stack.pop_back();
        } else if (tok.text == ";")
            dim_depth = 0; // `;` always ends any dimension context

        // --- K. Post-emit housekeeping ---
        // After emitting a token, update state for the NEXT token:
        //   • Open new indent levels for INDENT_OPEN keywords.
        //   • Schedule newlines (pending_nl) after block-opening keywords, `;`, etc.
        //   • Set flags for upcoming special tokens (case expr, control expr, struct).
        if (is_keyword(tok)) {
            // Count `do` depth so `end while` can be recognized as do…while tail.
            if (tok.lo == "do")
                ++do_depth;

            // After `case`/`casex`/`casez`/`caseinside`, expect `(expr)`.
            // When the `)` arrives we'll emit a newline before the case items.
            if (tok.lo == "case" || tok.lo == "casex" || tok.lo == "casez" ||
                tok.lo == "caseinside")
                case_expr_pending = true;

            // After `if`/`for`/`foreach`/`while`/`repeat`, expect `(cond)`.
            // When the `)` arrives we'll decide single-statement indentation.
            // `while` that closes a do…while (do_while_tail) is excluded.
            if (tok.lo == "if" || tok.lo == "for" || tok.lo == "foreach" ||
                (tok.lo == "while" && !do_while_tail) || tok.lo == "repeat")
                control_expr_pending = true;

            // `begin` cancels single_stmt_pending set by a preceding if/for/while,
            // because begin…end is its own indented block.
            if (tok.lo == "begin")
                single_stmt_pending = false;

            if (has(INDENT_OPEN, tok.lo)) {
                // Keywords that increase indentation for everything inside them.
                // Outmost design blocks use a configurable delta (default 1);
                // everything else adds exactly 1 level.
                int delta = (tok.lo == "module" || tok.lo == "macromodule" ||
                             tok.lo == "interface" || tok.lo == "package")
                                ? opts.default_indent_level_inside_outmost_block
                                : 1;
                indent_level += delta;
                indent_stack.push_back(delta);
                // Block-opening keywords (begin, fork, …) also schedule a newline
                // so the first statement inside appears on the next line.
                // `case` is handled separately via case_expr_pending.
                if (has(BLOCK_OPEN, tok.lo) && tok.lo != "case" && tok.lo != "casex" &&
                    tok.lo != "casez" && tok.lo != "caseinside")
                    pending_nl = true;
            } else if (has(INDENT_CLOSE, tok.lo)) {
                // After emitting `end`/`endmodule`/etc. schedule a newline so
                // the next statement starts on a fresh line.
                pending_nl = true;
            } else if (tok.lo == "struct" || tok.lo == "union") {
                // Next `{` should be treated as a struct/union brace (indented).
                struct_pend = true;
            }
        } else if (is_open_group(tok) && tok.text == "{") {
            if (struct_pend) {
                // `{` right after `struct`/`union` → indent the members inside.
                brace_stk.push_back("struct");
                pending_nl = true;
                indent_level += 1;
                indent_stack.push_back(1);
            } else {
                // `{` for concatenation, array literal, etc. → no extra indent.
                brace_stk.push_back("other");
            }
            struct_pend = false;
        } else if (is_close_group(tok) && tok.text == "}") {
            // Pop the brace stack (indent was already decremented in step G).
            if (!brace_stk.empty())
                brace_stk.pop_back();
        } else if (tok.text == ";") {
            // Semicolon = end of statement.
            // At top-level (paren_depth==0) schedule a newline after it.
            // Inside `for (init; cond; incr)` paren_depth > 0 → no newline.
            if (paren_depth == 0)
                pending_nl = true;
            // If we were in a single-statement body (no begin…end), this `;`
            // ends it → undo the extra indent level we added.
            if (single_stmt_active) {
                indent_level = std::max(0, indent_level - 1);
                single_stmt_active = false;
            }
        } else if (tok.directive) {
            // Compiler directives (`define, `include, `ifdef, …) always end
            // their logical line → schedule a newline after them.
            pending_nl = true;
        } else if (tok.comment) {
            // A comment that is followed by a newline in the original whitespace
            // → schedule a newline (the comment ends the line).
            if (i + 1 < tokens.size() && tokens[i + 1].whitespace &&
                tokens[i + 1].text.find('\n') != std::string::npos)
                pending_nl = true;
        } else if (is_identifier(tok)) {
            // Preprocessor conditionals:
            //   PP_COND_BARE: `else, `endif → no argument, newline immediately.
            //   PP_COND_WITH: `ifdef, `ifndef, `elsif → argument follows on same
            //                 line; set in_pp_cond so we newline after the arg.
            //   in_pp_cond + any other identifier → that was the argument → newline.
            if (has(PP_COND_BARE, tok.lo)) {
                pending_nl = true;
                in_pp_cond = false;
            } else if (has(PP_COND_WITH, tok.lo))
                in_pp_cond = true;
            else if (in_pp_cond) {
                pending_nl = true;
                in_pp_cond = false;
            }
        } else if (in_pp_cond) {
            // Non-identifier token after a `ifdef-style directive → treat it as
            // the end of the argument and schedule a newline.
            pending_nl = true;
            in_pp_cond = false;
        }

        prev_at_procedural = tok.text == "@" && prev && is_procedural_event_keyword(*prev);
        prev = &tok; // remember this token for next iteration's spacing decisions
    } // end main token loop

    // -----------------------------------------------------------------------
    // STEP 8: Finalize raw output
    //
    // If the last token didn't end with a newline, add one.
    // Collapse multiple trailing newlines to a single one.
    // -----------------------------------------------------------------------
    if (!at_bol) {
        out += '\n';
        ++fmt_line_count;
    }

    // Collapse extra trailing newlines to one
    while (out.size() >= 2 && out[out.size() - 1] == '\n' && out[out.size() - 2] == '\n')
        out.pop_back();

    // -----------------------------------------------------------------------
    // STEP 9: Convert to FormattedLine vector and run post-token pipeline
    //
    // Each line carries its syntactic kind directly, so downstream passes
    // can identify line roles (port decl, var decl, etc.) without a separate
    // line-number map.
    // -----------------------------------------------------------------------
    auto flines = text_to_lines(out);
    for (size_t fi = 0; fi < flines.size(); ++fi) {
        auto it = fmt_line_kinds.find((int)fi);
        if (it != fmt_line_kinds.end())
            flines[fi].kind = it->second;
    }
    flines = run_post_token_pipeline(std::move(flines), opts);
    out = render_lines(flines);

    // -----------------------------------------------------------------------
    // STEP 10: Final newline normalization
    //
    // Mirrors Python: result.rstrip('\n') + '\n'
    // -----------------------------------------------------------------------
    while (!out.empty() && out.back() == '\n')
        out.pop_back();
    out += '\n';

    // -----------------------------------------------------------------------
    // STEP 12: Safe-mode integrity check
    //
    // Verify that only whitespace changed.  This is a safety net against
    // formatter bugs that corrupt code.
    // -----------------------------------------------------------------------
    verify_safe_mode_unchanged(source, out, opts);

    return out;
}
