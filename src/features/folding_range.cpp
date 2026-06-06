#include "folding_range.hpp"
#include "document_state.hpp"
#include "formatter_lexer.hpp"
#include "formatter_token.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <string_view>
#include <tuple>
#include <utility>

#include <slang/syntax/AllSyntax.h>
#include <slang/syntax/SyntaxTree.h>
#include <slang/syntax/SyntaxVisitor.h>
#include <slang/text/SourceLocation.h>
#include <slang/text/SourceManager.h>

using namespace slang;
using namespace slang::syntax;
using namespace slang::parsing;

namespace {

// ── helpers shared by both paths ──────────────────────────────────────────

struct LineTable {
    std::string_view    text;
    std::vector<size_t> starts; // starts[i] = byte offset of line i

    LineTable() = default;
    explicit LineTable(std::string_view t) : text(t) {
        starts.push_back(0);
        for (size_t i = 0; i < t.size(); ++i)
            if (t[i] == '\n') starts.push_back(i + 1);
    }

    int line_of(size_t offset) const {
        auto it = std::upper_bound(starts.begin(), starts.end(), offset);
        return (int)(it - starts.begin()) - 1;
    }

    // Returns {start, end} byte offsets for line (end excludes '\n').
    std::pair<size_t, size_t> bounds(int line) const {
        if (line < 0 || (size_t)line >= starts.size())
            return {text.size(), text.size()};
        size_t s = starts[(size_t)line];
        size_t e = ((size_t)line + 1 < starts.size())
                       ? starts[(size_t)line + 1] - 1
                       : text.size();
        return {s, e};
    }

    int first_non_space_column(int line) const {
        auto [s, e] = bounds(line);
        for (size_t i = s; i < e; ++i)
            if (!std::isspace(static_cast<unsigned char>(text[i])))
                return (int)(i - s);
        return 0;
    }

    int line_length(int line) const {
        auto [s, e] = bounds(line);
        return (int)(e - s);
    }
};

// ── AST path helpers (used only by IfElseChainVisitor) ───────────────────

static int token_line(const SourceManager& sm, const Token& tok) {
    if (!tok || !tok.location().valid()) return -1;
    return (int)sm.getLineNumber(tok.location()) - 1;
}

static int token_column(const SourceManager& sm, const Token& tok) {
    if (!tok || !tok.location().valid()) return 0;
    return (int)sm.getColumnNumber(tok.location()) - 1;
}

static int first_line(const SourceManager& sm, const SyntaxNode& n) {
    return token_line(sm, n.getFirstToken());
}

static int last_line(const SourceManager& sm, const SyntaxNode& n) {
    return token_line(sm, n.getLastToken());
}

static void emit(std::vector<FoldingRange>& out, const SourceManager& sm, BufferID buffer,
                 int start, int end, const std::string& kind = "region") {
    if (start < 0 || end < 0 || start >= end) return;
    auto text_sv = sm.getSourceText(buffer);
    LineTable lt{text_sv};
    FoldingRange r;
    r.startLine      = start;
    r.endLine        = end;
    r.startCharacter = lt.first_non_space_column(start);
    r.endCharacter   = lt.line_length(end);
    r.kind           = kind;
    out.push_back(r);
}

static void emit_node(std::vector<FoldingRange>& out, const SourceManager& sm,
                      const SyntaxNode& node, const std::string& kind = "region") {
    Token first = node.getFirstToken();
    if (!first || !first.location().valid()) return;
    emit(out, sm, first.location().buffer(), first_line(sm, node), last_line(sm, node), kind);
}

static void emit_token_fold(std::vector<FoldingRange>& out, const LineTable& lt,
                            int start, int end, const std::string& kind = "region");

static std::optional<BufferID> find_current_buffer(const SourceManager& sm,
                                                   std::string_view text,
                                                   const std::string& uri) {
    std::string path = uri;
    if (path.rfind("file://", 0) == 0)
        path = path.substr(7);

    for (BufferID buffer : sm.getAllBuffers()) {
        if (sm.getIncludedFrom(buffer).valid())
            continue;
        if (sm.getSourceText(buffer) == text)
            return buffer;
        if (sm.getRawFileName(buffer) == uri || sm.getRawFileName(buffer) == path)
            return buffer;
        if (sm.getFullPath(buffer).string() == path)
            return buffer;
    }
    return std::nullopt;
}

// ── minimal AST visitor: if/else chain linking only ──────────────────────
//
// Token-based fold detection cannot reliably link chained if/else-if/else
// branches because it cannot distinguish them from independent blocks without
// parsing context.  This minimal visitor handles exactly two node types where
// AST precision is needed:
//   - ConditionalStatementSyntax: fold the whole if/else chain as one region
//   - IfGenerateSyntax: fold if-arm and else-arm as separate regions
//
// All other folds are produced by collect_token_folds() below.

struct IfElseChainVisitor : public SyntaxVisitor<IfElseChainVisitor> {
    const SourceManager&       sm;
    BufferID                   current_buffer;
    std::vector<FoldingRange>& out;

    IfElseChainVisitor(const SourceManager& sm, BufferID current_buffer,
                       std::vector<FoldingRange>& out)
        : sm(sm), current_buffer(current_buffer), out(out) {}

    bool in_current_buffer(const Token& tok) const {
        return tok && tok.location().valid() &&
               tok.location().buffer() == current_buffer;
    }

    void emit_node_if_current(const SyntaxNode& node) {
        Token first = node.getFirstToken();
        Token last  = node.getLastToken();
        if (!in_current_buffer(first) || !in_current_buffer(last))
            return;
        emit(out, sm, current_buffer, token_line(sm, first), token_line(sm, last));
    }

    void handle(const ConditionalStatementSyntax& node) {
        emit_node_if_current(node);
        visitDefault(node);
    }

    void handle(const IfGenerateSyntax& node) {
        Token first = node.getFirstToken();
        Token last  = node.getLastToken();
        if (!in_current_buffer(first) || !in_current_buffer(last)) {
            visitDefault(node);
            return;
        }
        int node_start = first_line(sm, node);
        int node_end   = last_line(sm, node);
        if (node.elseClause && in_current_buffer(node.elseClause->elseKeyword)) {
            int else_line = token_line(sm, node.elseClause->elseKeyword);
            emit(out, sm, current_buffer, node_start, else_line - 1);
            emit(out, sm, current_buffer, else_line, node_end);
        } else {
            emit(out, sm, current_buffer, node_start, node_end);
        }
        visitDefault(node);
    }
};

// AST-backed module/interface/program header list folds.
//
// The token scanner can usually split a parameterized ANSI header into "#(...)"
// and "(...)" folds, but some malformed / partially edited headers can still be
// parsed by slang as one header-shaped syntax region.  Use the current file AST
// as a second source of truth for the two delimiter pairs that users actually
// want to fold:
//
//     module m #(
//         parameter int W = 8
//     )(
//         input logic clk
//     );
//     ^^^^^^^^^^^^^^^^^ parameter list fold
//       ^^^^^^^^^^^^^^^ port list fold
//
// This intentionally does not emit a whole-header fold.  The enclosing module
// fold already covers the whole declaration when an endmodule exists, and a
// header-only fold such as (0, 11) competes with the more useful child folds in
// editors that show only one fold marker per start line.
struct HeaderListVisitor : public SyntaxVisitor<HeaderListVisitor> {
    const SourceManager&       sm;
    BufferID                   current_buffer;
    std::vector<FoldingRange>& out;

    HeaderListVisitor(const SourceManager& sm, BufferID current_buffer,
                      std::vector<FoldingRange>& out)
        : sm(sm), current_buffer(current_buffer), out(out) {}

    void emit_tokens(Token open, Token close) {
        if (!open || !close || !open.location().valid() ||
            !close.location().valid())
            return;
        if (open.location().buffer() != current_buffer ||
            close.location().buffer() != current_buffer)
            return;

        int start = token_line(sm, open);
        int end   = token_line(sm, close);
        if (start < 0 || end < 0 || start >= end)
            return;

        FoldingRange r;
        r.startLine      = start;
        r.endLine        = end;
        r.startCharacter = token_column(sm, open);
        r.endCharacter   = token_column(sm, close) + (int)close.rawText().length();
        r.kind           = "region";
        out.push_back(r);
    }

    void handle(const ModuleHeaderSyntax& node) {
        if (node.parameters)
            emit_tokens(node.parameters->openParen, node.parameters->closeParen);

        if (node.ports) {
            switch (node.ports->kind) {
            case SyntaxKind::AnsiPortList: {
                const auto& ports = node.ports->as<AnsiPortListSyntax>();
                emit_tokens(ports.openParen, ports.closeParen);
                break;
            }
            case SyntaxKind::NonAnsiPortList: {
                const auto& ports = node.ports->as<NonAnsiPortListSyntax>();
                emit_tokens(ports.openParen, ports.closeParen);
                break;
            }
            case SyntaxKind::WildcardPortList: {
                const auto& ports = node.ports->as<WildcardPortListSyntax>();
                emit_tokens(ports.openParen, ports.closeParen);
                break;
            }
            default:
                break;
            }
        }

        visitDefault(node);
    }
};

// AST-backed declaration collection.
//
// Token-only declaration detection is useful for inactive preprocessor branches,
// but it cannot safely recognize declarations whose type starts with an
// identifier:
//
//     my_type_t         state;
//     pkg::payload_t    payload;
//     some_class        handle;
//
// The same token shape can also be a module/interface instantiation, so guessing
// lexically would either miss valid declarations or fold instances as variables.
// For active code, slang's parsed syntax tree has already made that distinction,
// so this visitor records declaration statement extents directly from AST nodes.
struct DeclarationRunVisitor : public SyntaxVisitor<DeclarationRunVisitor> {
    const SourceManager& sm;
    BufferID current_buffer;
    std::vector<std::pair<int, int>>& decls;

    explicit DeclarationRunVisitor(const SourceManager& sm, BufferID current_buffer,
                                   std::vector<std::pair<int, int>>& decls)
        : sm(sm), current_buffer(current_buffer), decls(decls) {}

    void record(const SyntaxNode& node) {
        Token first = node.getFirstToken();
        Token last  = node.getLastToken();
        if (!first || !last || !first.location().valid() ||
            !last.location().valid())
            return;
        if (first.location().buffer() != current_buffer ||
            last.location().buffer() != current_buffer)
            return;
        int start = token_line(sm, first);
        int end   = token_line(sm, last);
        if (start >= 0 && end >= start)
            decls.emplace_back(start, end);
    }

    void handle(const DataDeclarationSyntax& node) {
        // Covers module/package/class data declarations, including user-defined
        // type names and class handles:
        //     my_type_t a;
        //     pkg::type_t b;
        //     my_class c;
        record(node);
        visitDefault(node);
    }

    void handle(const NetDeclarationSyntax& node) {
        record(node);
        visitDefault(node);
    }

    void handle(const UserDefinedNetDeclarationSyntax& node) {
        record(node);
        visitDefault(node);
    }

    void handle(const PortDeclarationSyntax& node) {
        // Non-ANSI module/interface headers declare only port names in the
        // header, then provide semicolon-terminated port declarations in the
        // body-like declaration area:
        //
        //     module m(clk, rst_n, data);
        //         input  logic       clk;
        //         input  logic       rst_n;
        //         output logic [7:0] data;
        //     endmodule
        //
        // These are declarations just like consecutive internal variables and
        // should participate in the same declaration-run folding.  Recording the
        // parsed AST node avoids lexical guesses and also handles typed ports
        // that use user-defined types:
        //
        //     output payload_t payload;
        //
        // ANSI header ports are parsed as header port syntax, not as these
        // semicolon-terminated PortDeclarationSyntax members, so this does not
        // create extra folds inside "(input ..., output ...)" headers.
        record(node);
        visitDefault(node);
    }

    void handle(const LocalVariableDeclarationSyntax& node) {
        // Procedural / assertion-local declarations.  This also catches
        // user-defined local variables inside begin/end blocks.
        record(node);
        visitDefault(node);
    }

    void handle(const ParameterDeclarationStatementSyntax& node) {
        // Semicolon-terminated parameter/localparam declarations in bodies.
        // Parameter port-list entries are ParameterDeclarationSyntax nodes under
        // ParameterPortListSyntax, not this statement wrapper, so recording only
        // the statement avoids folding individual entries inside "#(...)".
        record(node);
        visitDefault(node);
    }
};

static void collect_ast_declaration_folds(const SourceManager& sm,
                                          BufferID current_buffer,
                                          const LineTable& lt,
                                          const SyntaxNode& root,
                                          std::vector<FoldingRange>& out) {
    std::vector<std::pair<int, int>> decls;
    DeclarationRunVisitor v{sm, current_buffer, decls};
    root.visit(v);

    std::sort(decls.begin(), decls.end());
    decls.erase(std::unique(decls.begin(), decls.end()), decls.end());

    int run_start = -1;
    int run_end   = -1;
    auto flush = [&]() {
        if (run_start >= 0 && run_end > run_start)
            emit_token_fold(out, lt, run_start, run_end, "declarations");
        run_start = -1;
        run_end   = -1;
    };

    for (const auto& [start, end] : decls) {
        if (run_start < 0) {
            run_start = start;
            run_end   = end;
        } else if (start <= run_end + 1) {
            run_end = std::max(run_end, end);
        } else {
            flush();
            run_start = start;
            run_end   = end;
        }
    }
    flush();
}

// ── token path helpers ────────────────────────────────────────────────────

// Emit a fold using the LineTable for character-column computation.
// Used exclusively by collect_token_folds(); the AST post-pass uses emit().
static void emit_token_fold(std::vector<FoldingRange>& out, const LineTable& lt,
                            int start, int end, const std::string& kind) {
    if (start < 0 || end < 0 || start >= end) return;
    FoldingRange r;
    r.startLine      = start;
    r.endLine        = end;
    r.startCharacter = lt.first_non_space_column(start);
    r.endCharacter   = lt.line_length(end);
    r.kind           = kind;
    out.push_back(r);
}

// True if only whitespace precedes `offset` on its line.
// Used for comment role classification (own-line vs trailing).
static bool is_own_line_at_offset(std::string_view text, size_t offset) {
    if (offset > text.size()) return false;
    size_t pos = offset;
    while (pos > 0 && text[pos - 1] != '\n') --pos;
    for (size_t i = pos; i < offset; ++i) {
        if (!std::isspace(static_cast<unsigned char>(text[i]))) return false;
    }
    return true;
}

// ── unified token scan ────────────────────────────────────────────────────
//
// One sequential pass over the formatter's TokenStream handles ALL fold
// constructs for both active code and disabled preprocessor branches.
//
// This works because svfmt::TokenCollector does raw lexing without
// preprocessing: every token in the source — whether inside an active or
// inactive #ifdef branch — appears in the TokenStream with its real TokenKind.
//
// Preprocessor directive subtype classification uses lower_text prefix
// matching.  This is a justified CLAUDE.md exception: formatter_lexer.hpp
// collapses all preprocessor directives to TokenKind::Directive, so
// TokenKind cannot distinguish `ifdef from `endif from `celldefine.

static void collect_token_folds(const svfmt::TokenStream& tokens,
                                const LineTable& lt,
                                std::vector<FoldingRange>& out) {
    using TK = TokenKind;

    struct BraceRegion { int start_line{-1}; int outer_depth{0}; };
    struct ParenRegion  {
        int  start_line{-1};
        int  outer_depth{0};

        // True for a parameter value assignment / parameter port list opened by
        // "#(".  This extra bit lets us distinguish:
        //
        //     module m #( ... )( ... );
        //              ^ fold ^ then enable a second fold for the port list
        //
        // from an ordinary keyword-introduced parenthesized region.  Without
        // it, the first close-paren of a parameterized module consumes the
        // module keyword's one pending paren fold and the following ANSI port
        // list is missed.
        bool hash_from_header{false};
    };

    std::vector<int>         block_stack;
    std::vector<int>         case_stack;
    std::vector<int>         keyword_region_stack;
    std::vector<BraceRegion> brace_region_stack;
    std::vector<ParenRegion> paren_region_stack;
    std::vector<int>         pp_stack;
    std::vector<int>         cell_stack;

    int  pending_control_start{-1};
    int  pending_brace_region_start{-1};
    int  pending_bins_start{-1};
    bool pending_bins_equals{false};
    bool pending_paren_region{false};
    bool pending_hash_paren_from_header{false};
    int  brace_depth{0};
    int  paren_depth{0};

    int comment_run_start{-1};
    int comment_run_last{-1};
    int import_run_start{-1};
    int import_run_last{-1};
    int decl_run_start{-1};
    int decl_run_last{-1};
    int active_decl_start{-1};

    // `at_statement_start` is intentionally a light-weight lexical guard, not a
    // full parser.  Declaration folding only starts from the first meaningful
    // token of a semicolon-terminated statement.  This avoids treating type
    // keywords inside expressions, dimensions, or module headers as standalone
    // declarations while still working in active and inactive preprocessor
    // branches where AST nodes may be unavailable.
    bool at_statement_start{true};

    auto is_decl_start_keyword = [](TK kind) {
        switch (kind) {
        // Object lifetime / qualifiers that can lead variable declarations:
        //   static logic a;
        //   automatic int i;
        //   const var int c;
        //   rand bit [3:0] value;
        case TK::AutomaticKeyword:
        case TK::StaticKeyword:
        case TK::ConstKeyword:
        case TK::RandKeyword:
        case TK::RandCKeyword:
        case TK::VarKeyword:

        // Parameters are declarations too.  Parameter port lists are protected
        // by the paren-depth check at the call site, so these cases cover
        // semicolon-terminated declarations in module/package/class bodies.
        case TK::ParameterKeyword:
        case TK::LocalParamKeyword:

        // Built-in variable data types.
        case TK::LogicKeyword:
        case TK::RegKeyword:
        case TK::BitKeyword:
        case TK::ByteKeyword:
        case TK::ShortIntKeyword:
        case TK::IntKeyword:
        case TK::LongIntKeyword:
        case TK::IntegerKeyword:
        case TK::RealKeyword:
        case TK::RealTimeKeyword:
        case TK::ShortRealKeyword:
        case TK::TimeKeyword:
        case TK::StringKeyword:
        case TK::CHandleKeyword:
        case TK::EventKeyword:

        // Net declarations.  Keeping these with variable declarations matches
        // editor expectations: a run of signal declarations should fold
        // together regardless of whether individual lines use logic, wire,
        // tri, wand, supply0, etc.
        case TK::Supply0Keyword:
        case TK::Supply1Keyword:
        case TK::TriKeyword:
        case TK::TriAndKeyword:
        case TK::TriOrKeyword:
        case TK::TriRegKeyword:
        case TK::Tri0Keyword:
        case TK::Tri1Keyword:
        case TK::UWireKeyword:
        case TK::WireKeyword:
        case TK::WAndKeyword:
        case TK::WOrKeyword:
        case TK::InterconnectKeyword:
            return true;
        default:
            return false;
        }
    };

    auto flush_comment_run = [&]() {
        if (comment_run_start >= 0 && comment_run_last > comment_run_start)
            emit_token_fold(out, lt, comment_run_start, comment_run_last, "comment");
        comment_run_start = -1;
        comment_run_last  = -1;
    };

    auto flush_import_run = [&]() {
        if (import_run_start >= 0 && import_run_last > import_run_start)
            emit_token_fold(out, lt, import_run_start, import_run_last, "imports");
        import_run_start = -1;
        import_run_last  = -1;
    };

    auto flush_decl_run = [&]() {
        if (decl_run_start >= 0 && decl_run_last > decl_run_start)
            emit_token_fold(out, lt, decl_run_start, decl_run_last,
                            "declarations");
        decl_run_start = -1;
        decl_run_last  = -1;
    };

    auto finish_active_decl = [&](int end_line) {
        if (active_decl_start < 0) return;
        if (decl_run_start < 0) {
            decl_run_start = active_decl_start;
            decl_run_last  = end_line;
        } else if (active_decl_start <= decl_run_last + 1) {
            // Consecutive declaration statements fold as one run.  The <= form
            // also handles a multi-line declaration followed immediately by the
            // next declaration on the following line.
            decl_run_last = std::max(decl_run_last, end_line);
        } else {
            flush_decl_run();
            decl_run_start = active_decl_start;
            decl_run_last  = end_line;
        }
        active_decl_start = -1;
    };

    for (size_t i = 0; i < tokens.size(); ++i) {
        const auto& t = tokens[i];

        // Skip verbatim passthrough bodies from format-off/on regions.
        if (t.lex.is_disabled_region_body) continue;

        // ── comments ──────────────────────────────────────────────────────
        if (t.lex.comment_kind != svfmt::CommentLexemeKind::None) {
            if (active_decl_start < 0)
                flush_decl_run();
            size_t offset = t.lex.range.start().offset();
            int    line   = lt.line_of(offset);

            if (t.lex.comment_kind == svfmt::CommentLexemeKind::Block) {
                flush_comment_run();
                int newlines = (int)std::count(t.lex.text.begin(), t.lex.text.end(), '\n');
                // Only fold block comments that start on their own line.
                // Comment role classification may reference source positioning
                // (per CLAUDE.md exception for comment classification).
                if (newlines > 0 && is_own_line_at_offset(lt.text, offset))
                    emit_token_fold(out, lt, line, line + newlines, "comment");
            } else if (t.lex.comment_kind == svfmt::CommentLexemeKind::Line) {
                if (!is_own_line_at_offset(lt.text, offset)) {
                    // Trailing comment breaks the run so it does not fold
                    // together with any preceding own-line comment.
                    flush_comment_run();
                } else if (comment_run_start < 0) {
                    comment_run_start = line;
                    comment_run_last  = line;
                } else if (line == comment_run_last + 1) {
                    comment_run_last = line;
                } else {
                    flush_comment_run();
                    comment_run_start = line;
                    comment_run_last  = line;
                }
            }
            continue;
        }

        // ── preprocessor directives ────────────────────────────────────────
        // All directives share TokenKind::Directive in the formatter token
        // stream.  Subtype is classified via lower_text prefix matching.
        if (t.lex.is_directive) {
            if (active_decl_start < 0)
                flush_decl_run();
            flush_comment_run();
            // Directives between import statements do not break an import run
            // (mirrors the AST path where directives are trivia, not members).

            const std::string& ltext     = t.lex.lower_text;
            int                dir_line = lt.line_of(t.lex.range.start().offset());

            auto sw = [&](const char* prefix) {
                return ltext.rfind(prefix, 0) == 0;
            };

            if (sw("`ifdef") || sw("`ifndef")) {
                pp_stack.push_back(dir_line);
            } else if (sw("`elsif")) {
                if (!pp_stack.empty()) {
                    emit_token_fold(out, lt, pp_stack.back(), dir_line - 1);
                    pp_stack.back() = dir_line;
                }
            } else if (sw("`else") && !sw("`elsif")) {
                if (!pp_stack.empty()) {
                    emit_token_fold(out, lt, pp_stack.back(), dir_line - 1);
                    pp_stack.back() = dir_line;
                }
            } else if (sw("`endif")) {
                if (!pp_stack.empty()) {
                    emit_token_fold(out, lt, pp_stack.back(), dir_line);
                    pp_stack.pop_back();
                }
            } else if (sw("`celldefine")) {
                cell_stack.push_back(dir_line);
            } else if (sw("`endcelldefine")) {
                if (!cell_stack.empty()) {
                    emit_token_fold(out, lt, cell_stack.back(), dir_line);
                    cell_stack.pop_back();
                }
            }

            pending_control_start = -1;
            pending_paren_region  = false;
            at_statement_start    = true;
            continue;
        }

        // ── all other tokens ───────────────────────────────────────────────
        // Any non-comment, non-directive code token flushes the comment run.
        // Any such token that is not ImportKeyword also flushes the import run
        // (matches AST behavior where non-import members break import groups).
        flush_comment_run();
        if (t.lex.kind != TK::ImportKeyword)
            flush_import_run();

        const int token_line_number = lt.line_of(t.lex.range.start().offset());
        if (active_decl_start < 0 && at_statement_start &&
            paren_depth == 0 && brace_depth == 0 &&
            is_decl_start_keyword(t.lex.kind)) {
            active_decl_start = token_line_number;
        } else if (active_decl_start < 0 && at_statement_start &&
                   t.lex.kind != TK::Semicolon) {
            // A non-declaration statement breaks a declaration run.  For
            // example, do not fold declarations across an intervening assign,
            // always block, instance, or assertion.
            flush_decl_run();
        }

        switch (t.lex.kind) {

        // Control keywords: mark pending start for begin attribution
        case TK::AlwaysKeyword:
        case TK::AlwaysCombKeyword:
        case TK::AlwaysFFKeyword:
        case TK::AlwaysLatchKeyword:
        case TK::InitialKeyword:
        case TK::FinalKeyword:
        case TK::IfKeyword:
        case TK::ElseKeyword:
        case TK::DoKeyword:
        case TK::ForKeyword:
        case TK::ForeachKeyword:
        case TK::ForeverKeyword:
        case TK::WhileKeyword:
        case TK::RepeatKeyword: {
            int line = lt.line_of(t.lex.range.start().offset());
            if (line >= 0) pending_control_start = line;
            break;
        }

        // Keyword-region open: push start line, next ( may be a port list
        case TK::ForkKeyword:
        case TK::GenerateKeyword:
        case TK::FunctionKeyword:
        case TK::TaskKeyword:
        case TK::ClassKeyword:
        case TK::CoverGroupKeyword:
        case TK::PropertyKeyword:
        case TK::SequenceKeyword:
        case TK::CheckerKeyword:
        case TK::PrimitiveKeyword:
        case TK::ConfigKeyword:
        case TK::SpecifyKeyword:
        case TK::PackageKeyword:
        case TK::InterfaceKeyword:
        case TK::ProgramKeyword:
        case TK::ModuleKeyword:
        case TK::MacromoduleKeyword:
        case TK::ClockingKeyword: {
            int line = lt.line_of(t.lex.range.start().offset());
            if (line >= 0) keyword_region_stack.push_back(line);
            pending_control_start = -1;
            pending_paren_region  = true;
            break;
        }

        // Keyword-region close: pop and emit
        // ModuleKeyword and MacromoduleKeyword both close with EndModuleKeyword.
        case TK::JoinKeyword:
        case TK::JoinAnyKeyword:
        case TK::JoinNoneKeyword:
        case TK::EndGenerateKeyword:
        case TK::EndFunctionKeyword:
        case TK::EndTaskKeyword:
        case TK::EndClassKeyword:
        case TK::EndGroupKeyword:
        case TK::EndPropertyKeyword:
        case TK::EndSequenceKeyword:
        case TK::EndCheckerKeyword:
        case TK::EndPrimitiveKeyword:
        case TK::EndConfigKeyword:
        case TK::EndSpecifyKeyword:
        case TK::EndPackageKeyword:
        case TK::EndInterfaceKeyword:
        case TK::EndProgramKeyword:
        case TK::EndModuleKeyword:
        case TK::EndClockingKeyword: {
            int end_line = lt.line_of(t.lex.range.start().offset());
            if (!keyword_region_stack.empty()) {
                emit_token_fold(out, lt, keyword_region_stack.back(), end_line);
                keyword_region_stack.pop_back();
            }
            pending_control_start = -1;
            pending_paren_region  = false;
            break;
        }

        // Brace-region starters (constraint / coverpoint / cross / typedef)
        // TypedefKeyword anchors the fold start so "typedef struct {" folds
        // from the typedef line rather than the struct/enum/union line.
        case TK::TypedefKeyword: {
            int line = lt.line_of(t.lex.range.start().offset());
            if (line >= 0) pending_brace_region_start = line;
            pending_control_start = -1;
            break;
        }

        case TK::ConstraintKeyword:
        case TK::CoverPointKeyword:
        case TK::CrossKeyword: {
            int line = lt.line_of(t.lex.range.start().offset());
            if (line >= 0) pending_brace_region_start = line;
            pending_control_start = -1;
            break;
        }

        // enum/struct/union brace bodies fold as regions.  Only set pending
        // when not already pending so a preceding TypedefKeyword wins as the
        // fold start line (e.g. "typedef struct {" folds from "typedef").
        case TK::EnumKeyword:
        case TK::StructKeyword:
        case TK::UnionKeyword: {
            int line = lt.line_of(t.lex.range.start().offset());
            if (line >= 0 && pending_brace_region_start < 0)
                pending_brace_region_start = line;
            pending_control_start = -1;
            break;
        }

        // Bins keywords: wait for = before tracking the brace region
        case TK::BinsKeyword:
        case TK::IllegalBinsKeyword:
        case TK::IgnoreBinsKeyword: {
            int line = lt.line_of(t.lex.range.start().offset());
            if (line >= 0) {
                pending_bins_start  = line;
                pending_bins_equals = false;
            }
            pending_control_start = -1;
            break;
        }

        case TK::Equals:
            if (pending_bins_start >= 0) pending_bins_equals = true;
            break;

        case TK::OpenBrace:
            if (pending_brace_region_start >= 0) {
                brace_region_stack.push_back({pending_brace_region_start, brace_depth});
                pending_brace_region_start = -1;
            } else if (pending_bins_start >= 0 && pending_bins_equals) {
                brace_region_stack.push_back({pending_bins_start, brace_depth});
                pending_bins_start  = -1;
                pending_bins_equals = false;
            }
            ++brace_depth;
            pending_control_start = -1;
            break;

        case TK::CloseBrace: {
            int close_line = lt.line_of(t.lex.range.start().offset());
            if (brace_depth > 0) --brace_depth;
            if (!brace_region_stack.empty() &&
                brace_region_stack.back().outer_depth == brace_depth) {
                emit_token_fold(out, lt,
                                brace_region_stack.back().start_line, close_line);
                brace_region_stack.pop_back();
            }
            pending_control_start = -1;
            break;
        }

        // Array literal '{...}: increment brace depth without starting a new
        // brace region.  Without this, inner apostrophe-braces inside a
        // constraint { } block would miscount depth and emit folds at wrong
        // boundaries.
        case TK::ApostropheOpenBrace:
            ++brace_depth;
            pending_control_start = -1;
            break;

        case TK::CaseKeyword:
        case TK::CaseXKeyword:
        case TK::CaseZKeyword:
        case TK::RandCaseKeyword: {
            int case_line = lt.line_of(t.lex.range.start().offset());
            if (case_line >= 0) case_stack.push_back(case_line);
            pending_control_start = -1;
            break;
        }

        case TK::EndCaseKeyword: {
            int endcase_line = lt.line_of(t.lex.range.start().offset());
            if (!case_stack.empty()) {
                emit_token_fold(out, lt, case_stack.back(), endcase_line);
                case_stack.pop_back();
            }
            pending_control_start = -1;
            break;
        }

        case TK::BeginKeyword: {
            int begin_line = lt.line_of(t.lex.range.start().offset());
            block_stack.push_back(
                pending_control_start >= 0 ? pending_control_start : begin_line);
            pending_control_start = -1;
            break;
        }

        case TK::EndKeyword: {
            int end_line = lt.line_of(t.lex.range.start().offset());
            if (!block_stack.empty()) {
                emit_token_fold(out, lt, block_stack.back(), end_line);
                block_stack.pop_back();
            }
            pending_control_start = -1;
            break;
        }

        // #( introduces a parameter value assignment paren region
        case TK::Hash: {
            if (i + 1 < tokens.size() &&
                tokens[i + 1].lex.kind == TK::OpenParenthesis) {
                // If a keyword header (module/interface/program/etc.) was
                // waiting for its first parenthesized region and sees "#(",
                // the hash paren is the parameter list.  After that closes,
                // the same header may still have an ANSI port list to fold.
                pending_hash_paren_from_header = pending_paren_region;
                pending_paren_region = true;
            }
            break;
        }

        // Parenthesized port/parameter list regions.
        // pending_paren_region is set by module/function/task/interface/program/
        // checker/primitive keywords (and #) so only the first ( after such a
        // keyword starts a fold.  This covers ANSI port lists, function/task
        // parameter lists, and parameter value assignments.  Instantiation
        // connection lists are not detected here because instance names are
        // identifiers, not keywords.
        case TK::OpenParenthesis: {
            int open_line = lt.line_of(t.lex.range.start().offset());
            if (pending_paren_region) {
                paren_region_stack.push_back(
                    {open_line, paren_depth, pending_hash_paren_from_header});
                pending_paren_region = false;
                pending_hash_paren_from_header = false;
            }
            ++paren_depth;
            pending_control_start = -1;
            at_statement_start = false;
            break;
        }

        case TK::CloseParenthesis: {
            if (paren_depth > 0) --paren_depth;
            int close_line = lt.line_of(t.lex.range.start().offset());
            if (!paren_region_stack.empty() &&
                paren_region_stack.back().outer_depth == paren_depth) {
                bool hash_from_header = paren_region_stack.back().hash_from_header;
                emit_token_fold(out, lt,
                                paren_region_stack.back().start_line, close_line);
                paren_region_stack.pop_back();

                // Parameterized module/interface/program/checker/primitive
                // headers have two adjacent parenthesized regions:
                //   #(parameter ...)
                //   (input ..., output ...)
                // Re-arm exactly after a header "#(...)" so the port list is
                // offered as its own fold in addition to the parameter list.
                if (hash_from_header)
                    pending_paren_region = true;
            }
            pending_control_start = -1;
            at_statement_start = false;
            break;
        }

        // Import run grouping.
        // ImportKeyword covers both package imports and DPI imports.
        // DPI imports (next non-trivia token is a StringLiteral) are excluded
        // from run grouping.  The run flushes when a non-adjacent import or
        // any non-import code token is encountered.
        case TK::ImportKeyword: {
            // Look ahead past comments/unknowns to the first content token.
            size_t next = i + 1;
            while (next < tokens.size() &&
                   (tokens[next].lex.comment_kind != svfmt::CommentLexemeKind::None ||
                    tokens[next].lex.kind == TK::Unknown ||
                    tokens[next].lex.is_disabled_region_body))
                ++next;

            if (next < tokens.size() &&
                tokens[next].lex.kind == TK::StringLiteral) {
                // DPI import: skip to closing semicolon, do not track.
                while (i < tokens.size() && tokens[i].lex.kind != TK::Semicolon)
                    ++i;
                // Reset state as if we processed the semicolon.
                pending_control_start          = -1;
                pending_brace_region_start     = -1;
                pending_bins_start             = -1;
                pending_bins_equals            = false;
                pending_paren_region           = false;
                pending_hash_paren_from_header = false;
                at_statement_start             = true;
                break;
            }

            // Package import: extent is from ImportKeyword to its Semicolon.
            int    import_start_line =
                lt.line_of(t.lex.range.start().offset());
            size_t semi = i + 1;
            while (semi < tokens.size() && tokens[semi].lex.kind != TK::Semicolon)
                ++semi;
            int import_end_line = semi < tokens.size()
                ? lt.line_of(tokens[semi].lex.range.start().offset())
                : import_start_line;

            if (import_run_start < 0) {
                import_run_start = import_start_line;
                import_run_last  = import_end_line;
            } else if (import_start_line == import_run_last + 1) {
                import_run_last = import_end_line;
            } else {
                flush_import_run();
                import_run_start = import_start_line;
                import_run_last  = import_end_line;
            }
            i = semi; // advance past the import statement
            // Reset state as if we processed the semicolon.
            pending_control_start          = -1;
            pending_brace_region_start     = -1;
            pending_bins_start             = -1;
            pending_bins_equals            = false;
            pending_paren_region           = false;
            pending_hash_paren_from_header = false;
            at_statement_start             = true;
            break;
        }

        case TK::Semicolon:
            finish_active_decl(lt.line_of(t.lex.range.start().offset()));
            pending_control_start      = -1;
            pending_brace_region_start = -1;
            pending_bins_start         = -1;
            pending_bins_equals        = false;
            pending_paren_region       = false;
            pending_hash_paren_from_header = false;
            at_statement_start        = true;
            break;

        default:
            pending_control_start = -1;
            at_statement_start = false;
            break;
        }
    }

    flush_comment_run();
    flush_import_run();
    flush_decl_run();
}

// ── normalization ─────────────────────────────────────────────────────────

static void normalize_folds(std::vector<FoldingRange>& folds, const LineTable& lt) {
    std::sort(folds.begin(), folds.end(), [](const FoldingRange& a, const FoldingRange& b) {
        return std::tie(a.startLine, a.endLine, a.kind, a.startCharacter, a.endCharacter) <
               std::tie(b.startLine, b.endLine, b.kind, b.startCharacter, b.endCharacter);
    });

    folds.erase(
        std::unique(folds.begin(), folds.end(),
                    [](const FoldingRange& a, const FoldingRange& b) {
                        return a.startLine == b.startLine && a.endLine == b.endLine &&
                               a.kind == b.kind && a.startCharacter == b.startCharacter &&
                               a.endCharacter == b.endCharacter;
                    }),
        folds.end());

    // If the token pass and AST pass both found the same line range, prefer the
    // more precise AST delimiter columns over the token pass's line-indentation
    // columns.  This matters for parameterized module headers where a coarse
    // line-start range on the module line can compete with the enclosing module
    // fold in clients that pick one fold marker per line.
    std::vector<FoldingRange> column_pruned;
    column_pruned.reserve(folds.size());
    for (const auto& candidate : folds) {
        auto same_lines = [&](const FoldingRange& existing) {
            return existing.startLine == candidate.startLine &&
                   existing.endLine == candidate.endLine &&
                   existing.kind == candidate.kind;
        };

        auto it = std::find_if(column_pruned.begin(), column_pruned.end(), same_lines);
        if (it == column_pruned.end()) {
            column_pruned.push_back(candidate);
        } else {
            if (candidate.startCharacter > it->startCharacter ||
                (candidate.startCharacter == it->startCharacter &&
                 candidate.endCharacter > it->endCharacter))
                *it = candidate;
        }
    }
    folds.swap(column_pruned);

    // Drop redundant "whole header" style folds when the useful split folds
    // already cover the exact same span.  Some clients effectively expose only
    // one fold marker per start line, so keeping (0,11) beside (0,4)+(4,11)
    // can hide the parameter-list fold from users.
    std::vector<FoldingRange> no_redundant_headers;
    no_redundant_headers.reserve(folds.size());
    for (size_t i = 0; i < folds.size(); ++i) {
        const auto& outer = folds[i];
        bool redundant_header = false;
        if (outer.kind == "region") {
            for (const auto& left : folds) {
                if (left.kind != "region" || left.endLine <= left.startLine ||
                    left.startLine != outer.startLine ||
                    left.endLine >= outer.endLine) continue;
                for (const auto& right : folds) {
                    if (right.kind == "region" &&
                        right.startLine == left.endLine &&
                        right.endLine == outer.endLine &&
                        right.endLine > right.startLine) {
                        redundant_header = true;
                        break;
                    }
                }
            }
        }
        if (!redundant_header)
            no_redundant_headers.push_back(outer);
    }
    folds.swap(no_redundant_headers);

    // Neovim's built-in LSP fold expression currently projects LSP ranges onto
    // Vim's older line-based fold model.  That model cannot keep adjacent ranges
    // that share a delimiter line independent:
    //
    //     module m #(          // parameter fold starts here
    //         parameter int W = 8
    //     )(                   // parameter ends and port starts on this line
    //         input logic clk
    //     );                   // port ends and module body starts here
    //
    // Exact LSP ranges are semantically nice:
    //     #(...)  => [module line, ")(" line]
    //     (...)   => [")(" line, ");" line]
    //     module  => [");" line, endmodule line]
    //
    // But in Neovim those touching child ranges become one continuous header
    // fold.  Make parameterized header children line-compatible by leaving the
    // shared delimiter lines outside the child folds:
    //     #(...)  => [module line, line before ")("]
    //     (...)   => [line after ")(", line before ");"]
    //     module  => remains the full module [module line, endmodule line]
    //
    // Keeping the enclosing module as the full declaration is important for
    // users who close a fold from inside the module body: that operation should
    // collapse the whole module, not only the text after the port list.
    //
    // This is deliberately applied only to the distinctive header shape where a
    // region starts later on the same line as an enclosing region (the "#(" on a
    // module/interface/program/checker/primitive header) and the next region
    // starts on the previous region's close line.
    for (size_t li = 0; li < folds.size(); ++li) {
        auto& left = folds[li];
        if (left.kind != "region" || left.startCharacter <= 0 ||
            left.endLine <= left.startLine)
            continue;

        for (size_t ri = 0; ri < folds.size(); ++ri) {
            if (li == ri)
                continue;
            auto& right = folds[ri];
            if (right.kind != "region" || right.startLine != left.endLine ||
                right.startCharacter >= left.startCharacter ||
                right.endLine <= right.startLine)
                continue;

            const int boundary_line   = left.endLine;  // the ")(" line
            const int terminator_line = right.endLine; // the ");" line

            const int left_end   = boundary_line - 1;
            const int right_start = boundary_line + 1;
            const int right_end   = terminator_line - 1;

            // Keep only meaningful multi-line folds.  If a tiny header has no
            // interior lines after removing delimiter lines, the invalid range
            // is pruned below instead of exposing a misleading one-line fold.
            left.endLine      = left_end;
            left.endCharacter = lt.line_length(left_end);

            right.startLine      = right_start;
            right.startCharacter = lt.first_non_space_column(right_start);
            right.endLine        = right_end;
            right.endCharacter   = lt.line_length(right_end);
        }
    }

    folds.erase(std::remove_if(folds.begin(), folds.end(),
                               [](const FoldingRange& r) {
                                   return r.startLine < 0 || r.endLine < 0 ||
                                          r.startLine >= r.endLine;
                               }),
                folds.end());

    // Declaration folds can be produced by two complementary sources:
    //   - token scan: works in inactive preprocessor branches, but only for
    //     keyword-led declarations;
    //   - AST scan: works for active code and understands user-defined types.
    //
    // When active code contains only keyword-led declarations, both paths can
    // produce the same or overlapping declaration range.  Keep the widest range
    // for each overlap group so clients do not show redundant nested folds like
    // [1,2] inside [1,3] for one consecutive declaration section.
    std::vector<FoldingRange> pruned;
    pruned.reserve(folds.size());
    for (size_t i = 0; i < folds.size(); ++i) {
        const auto& current = folds[i];
        bool contained_in_declaration = false;
        if (current.kind == "declarations") {
            for (size_t j = 0; j < folds.size(); ++j) {
                if (i == j || folds[j].kind != "declarations")
                    continue;
                const bool contained =
                    folds[j].startLine <= current.startLine &&
                    folds[j].endLine >= current.endLine &&
                    (folds[j].startLine != current.startLine ||
                     folds[j].endLine != current.endLine);
                if (contained) {
                    contained_in_declaration = true;
                    break;
                }
            }
        }
        if (!contained_in_declaration)
            pruned.push_back(current);
    }
    folds.swap(pruned);
}

} // namespace

// ── public API ────────────────────────────────────────────────────────────

std::vector<FoldingRange> provide_folding_range(const Analyzer& analyzer,
                                                const FoldingRangeRequestParams& params) {
    auto state = analyzer.get_state(params.textDocument.uri.raw_uri_);
    if (!state || !state->tree) return {};

    std::vector<FoldingRange> out;

    // Primary path: unified token scan over the formatter's TokenStream.
    // TokenCollector does raw lexing without preprocessing, so tokens from
    // both active and inactive preprocessor branches appear in the stream.
    FormatOptions default_opts;
    svfmt::TokenStream tokens =
        svfmt::TokenCollector(state->text, default_opts).collect();
    LineTable lt{state->text};
    collect_token_folds(tokens, lt, out);

    // AST post-passes use only syntax nodes whose source tokens belong to the
    // opened document buffer.  Included files can appear in slang's current
    // SyntaxTree, but LSP folding ranges must always be expressed in the
    // requested document's line coordinates.
    if (state->source_manager) {
        auto current_buffer = find_current_buffer(*state->source_manager, state->text,
                                                  params.textDocument.uri.raw_uri_);
        if (current_buffer) {
            collect_ast_declaration_folds(*state->source_manager, *current_buffer,
                                          lt, state->tree->root(), out);

            HeaderListVisitor h{*state->source_manager, *current_buffer, out};
            state->tree->root().visit(h);

            // ConditionalStatementSyntax and IfGenerateSyntax require AST
            // precision to link chained branches; token-only detection cannot
            // reliably do this.  These visitors currently use emit(), so keep
            // them after the current-buffer lookup succeeds.
            IfElseChainVisitor v{*state->source_manager, *current_buffer, out};
            state->tree->root().visit(v);
        }
    }

    normalize_folds(out, lt);
    return out;
}
