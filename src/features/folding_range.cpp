#include "folding_range.hpp"
#include "document_state.hpp"
#include "formatter_lexer.hpp"
#include "formatter_token.hpp"
#include "string_utils.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <map>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <slang/syntax/AllSyntax.h>

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

    // Both of these answer in UTF-16 code units, because that is what LSP
    // measures `FoldingRange.startCharacter` / `endCharacter` in -- the same
    // encoding every other position this server emits goes through
    // `utf16_column()` to reach.  Folds were the exception and reported byte
    // counts, so a fold whose first or last line carried any non-ASCII text
    // named a column past the end of that line: `  end // <CJK comment>` is 16
    // UTF-16 units and was reported as 30.
    //
    // Neovim sends `lineFoldingOnly` and ignores both fields, which is why this
    // stayed invisible; a client that places the fold marker by column does not.

    int first_non_space_column(int line) const {
        auto [s, e] = bounds(line);
        for (size_t i = s; i < e; ++i)
            if (!std::isspace(static_cast<unsigned char>(text[i])))
                return (int)utf16_length(text.substr(s, i - s));
        return 0;
    }

    int line_length(int line) const {
        auto [s, e] = bounds(line);
        return (int)utf16_length(text.substr(s, e - s));
    }

    // Index of the last line the buffer actually holds.  `starts` gains an
    // entry past a trailing '\n', but that is not a line the editor has:
    // "a\nb\n" is two lines, not three.
    int last_line() const {
        if (starts.size() <= 1) return 0;
        if (!text.empty() && text.back() == '\n') return (int)starts.size() - 2;
        return (int)starts.size() - 1;
    }
};

// Emit a fold.  The LineTable is built once per request and handed in rather
// than derived from the buffer here: it is a full scan of the file, and this is
// called once per fold, so building it here made producing N folds for a file of
// M bytes cost O(N x M).  On a 13k-line RTL file that was ~80 ms of a ~170 ms
// foldingRange -- paid on every keystroke, because the editor re-requests folds
// on every didChange.
static void emit(std::vector<FoldingRange>& out, const LineTable& lt,
                 int start, int end, const std::string& kind = "region") {
    // Never hand back a range the buffer does not have.  An unterminated block
    // comment produced exactly that: its lexeme runs to EOF, so it carries the
    // file's final newline and its line count was one too many.  Clamping here
    // rather than at each emit site covers every construct a truncated buffer
    // can leave open.
    end = std::min(end, lt.last_line());
    if (start < 0 || end < 0 || start >= end) return;
    FoldingRange r;
    r.startLine      = start;
    r.endLine        = end;
    r.startCharacter = lt.first_non_space_column(start);
    r.endCharacter   = lt.line_length(end);
    r.kind           = kind;
    out.push_back(r);
}

// ── token path helpers ────────────────────────────────────────────────────

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

// ── folding's own token scan ──────────────────────────────────────────────
//
// Folding reads five facts per token: kind, comment kind, directive kind, the
// byte offset it starts at, and (for block comments) its text.  It used to get
// them from svfmt::TokenCollector, the formatter's collector, which also
// materializes a std::string of every token's text, the immutable input-trivia
// facts every formatting pass reads (column, indent, blank lines before), and a
// mutable metadata block per token for each of the eight passes.  Folding reads
// none of that.
//
// This scan produces the five facts and nothing else, with the same token
// boundaries and the same three frozen spans — a directive line, a multiline
// `define body, a format-off region — so the folds it feeds are unchanged.
// clangd's getFoldingRanges does the same thing for the same reason: it runs a
// pseudoparser over a raw token stream rather than over structure built for
// another feature.

enum class CommentKind : uint8_t { None, Line, Block };

struct FoldToken {
    std::string_view text;                     // into the request's own text
    uint32_t         start{0};                 // byte offset in that text
    TokenKind        kind{TokenKind::Unknown};
    CommentKind      comment_kind{CommentKind::None};
    SyntaxKind       directive_kind{SyntaxKind::Unknown};
};

/// One-past-the-end of the multiline `define block starting at 'start', or 0
/// when that define is a single line.
size_t multiline_define_end(std::string_view src, size_t start) {
    size_t eol = src.find('\n', start);
    if (eol == std::string_view::npos)
        return 0;
    size_t check = eol;
    while (check > start && (src[check - 1] == ' ' || src[check - 1] == '\t'))
        --check;
    if (check == start || src[check - 1] != '\\')
        return 0;

    size_t pos = eol + 1;
    while (pos < src.size()) {
        size_t next_eol  = src.find('\n', pos);
        size_t line_end  = (next_eol == std::string_view::npos) ? src.size() : next_eol;
        size_t chk       = line_end;
        while (chk > pos && (src[chk - 1] == ' ' || src[chk - 1] == '\t'))
            --chk;
        bool has_cont = (chk > pos && src[chk - 1] == '\\');
        pos = (next_eol == std::string_view::npos) ? src.size() : next_eol + 1;
        if (!has_cont)
            break;
    }
    return pos;
}

std::vector<FoldToken> lex_fold_tokens(std::string_view src) {
    std::vector<FoldToken> out;
    if (src.empty())
        return out;
    // SystemVerilog runs about one token per four bytes of source.  One
    // reservation up front beats a dozen reallocations of a vector this long.
    out.reserve(src.size() / 4 + 16);

    // Folding is not configurable, so these are always the default patterns;
    // the regexes behind them are cached per pattern across requests.
    const FormatOptions opts;
    const auto off_re = svfmt::cached_format_marker_regex(opts.format_off_comment_pattern);
    const auto on_re  = svfmt::cached_format_marker_regex(opts.format_on_comment_pattern);

    slang::SourceManager  sm;
    slang::BumpAllocator  alloc;
    slang::Diagnostics    diagnostics;
    auto                  buffer = sm.assignText(src);
    slang::parsing::Lexer lexer(buffer, alloc, diagnostics, sm);

    size_t frozen_end = 0;     // one-past a directive line or `define body
    bool   disabled   = false; // inside a format-off region

    auto push = [&](TokenKind kind, size_t pos, size_t len, CommentKind comment_kind,
                    SyntaxKind directive_kind) {
        FoldToken tok;
        tok.text           = src.substr(pos, len);
        tok.start          = static_cast<uint32_t>(pos);
        tok.kind           = kind;
        tok.comment_kind   = comment_kind;
        tok.directive_kind = directive_kind;
        out.push_back(tok);
    };

    while (true) {
        const slang::parsing::Token token = lexer.lex();
        const bool                  eof   = token.kind == TokenKind::EndOfFile;

        // Comments reach a raw lex as leading trivia.  Trivia carries no
        // location of its own but sits immediately before its parent token in
        // source order, so derive each offset from the parent's rather than
        // searching for the raw text — searching is ambiguous for repeated
        // comments.  EOF's trivia is collected before breaking out, because a
        // trailing `endmodule // name` comment hangs off it.
        size_t token_pos = token.location().valid() ? token.location().offset() : src.size();
        token_pos        = std::min(token_pos, src.size());
        size_t trivia_total = 0;
        for (const auto& trivia : token.trivia())
            trivia_total += trivia.getRawText().size();
        size_t trivia_pos = token_pos >= trivia_total ? token_pos - trivia_total : 0;

        for (const auto& trivia : token.trivia()) {
            const size_t      len = trivia.getRawText().size();
            const CommentKind comment_kind =
                trivia.kind == slang::parsing::TriviaKind::LineComment    ? CommentKind::Line
                : trivia.kind == slang::parsing::TriviaKind::BlockComment ? CommentKind::Block
                                                                          : CommentKind::None;
            if (comment_kind != CommentKind::None && trivia_pos >= frozen_end) {
                const std::string_view raw = src.substr(trivia_pos, len);
                if (disabled) {
                    // The marker that ends a format-off region belongs to the
                    // region, so it closes it without folding as a comment of
                    // its own — the same boundary the collector drew.
                    if (svfmt::is_format_marker(raw, on_re, opts.format_on_comment_pattern))
                        disabled = false;
                }
                else {
                    push(TokenKind::Unknown, trivia_pos, len, comment_kind,
                         SyntaxKind::Unknown);
                    if (svfmt::is_format_marker(raw, off_re, opts.format_off_comment_pattern))
                        disabled = true;
                }
            }
            trivia_pos += len;
        }

        if (eof)
            break;
        if (!token.location().valid())
            continue;

        const size_t pos = token.location().offset();
        if (pos < frozen_end || disabled)
            continue;

        if (token.kind == TokenKind::Directive) {
            const SyntaxKind directive_kind = token.directiveKind();

            // A user macro invocation is an ordinary token, not a directive
            // line: the parens of `` `uvm_info(a, b, c) `` have to stay visible
            // to the paren folds below.
            if (directive_kind == SyntaxKind::MacroUsage) {
                push(TokenKind::MacroUsage, pos, token.rawText().size(), CommentKind::None,
                     SyntaxKind::Unknown);
                continue;
            }

            // A multiline `define body is one token; every other directive runs
            // to the end of its line.  Slang lexes `ifdef and its operand
            // separately, and folding must not read that operand as the start of
            // a declaration run.
            size_t end = directive_kind == SyntaxKind::DefineDirective
                             ? multiline_define_end(src, pos)
                             : 0;
            if (end <= pos) {
                end = src.find('\n', pos);
                if (end == std::string_view::npos)
                    end = src.size();
            }
            push(TokenKind::Directive, pos, end - pos, CommentKind::None, directive_kind);
            frozen_end = end;
            continue;
        }

        push(token.kind, pos, token.rawText().size(), CommentKind::None, SyntaxKind::Unknown);
    }
    return out;
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
// Preprocessor directives all share TokenKind::Directive, but the formatter
// lexer preserves slang's precise Token::directiveKind() as an immutable fact.
// Folding uses that subtype so directive-specific behavior does not depend on
// raw spelling/string matching.

static void collect_token_folds(const std::vector<FoldToken>& tokens,
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
    struct PreprocessorFrame {
        int    branch_start_line{-1};
        size_t block_stack_size{0};
        size_t case_stack_size{0};
        size_t keyword_region_stack_size{0};
        size_t brace_region_stack_size{0};
        size_t paren_region_stack_size{0};
        int    brace_depth{0};
        int    paren_depth{0};
    };

    std::vector<PreprocessorFrame> pp_stack;
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

    auto make_pp_frame = [&](int branch_start_line) {
        return PreprocessorFrame{
            .branch_start_line = branch_start_line,
            .block_stack_size = block_stack.size(),
            .case_stack_size = case_stack.size(),
            .keyword_region_stack_size = keyword_region_stack.size(),
            .brace_region_stack_size = brace_region_stack.size(),
            .paren_region_stack_size = paren_region_stack.size(),
            .brace_depth = brace_depth,
            .paren_depth = paren_depth,
        };
    };

    auto restore_to_pp_frame = [&](const PreprocessorFrame& frame) {
        // A structural region opened inside one preprocessor branch must not be
        // matched by a close token from a sibling branch.  Example:
        //
        //   `ifdef FOO
        //     task req_data();
        //   `elsif BAR
        //     endtask
        //   `endif
        //
        // The task header and endtask are mutually exclusive after
        // preprocessing.  On every branch boundary, discard only the structural
        // state created since the matching `ifdef/`ifndef.  Outer regions that
        // existed before the conditional, such as a surrounding module or task,
        // remain on their stacks so they can still fold across the whole
        // conditional.
        block_stack.resize(std::min(block_stack.size(), frame.block_stack_size));
        case_stack.resize(std::min(case_stack.size(), frame.case_stack_size));
        keyword_region_stack.resize(std::min(keyword_region_stack.size(),
                                             frame.keyword_region_stack_size));
        brace_region_stack.resize(std::min(brace_region_stack.size(),
                                           frame.brace_region_stack_size));
        paren_region_stack.resize(std::min(paren_region_stack.size(),
                                           frame.paren_region_stack_size));
        brace_depth = frame.brace_depth;
        paren_depth = frame.paren_depth;

        pending_control_start = -1;
        pending_brace_region_start = -1;
        pending_bins_start = -1;
        pending_bins_equals = false;
        pending_paren_region = false;
        pending_hash_paren_from_header = false;
        active_decl_start = -1;
        at_statement_start = true;
    };

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
            emit(out, lt, comment_run_start, comment_run_last, "comment");
        comment_run_start = -1;
        comment_run_last  = -1;
    };

    auto flush_import_run = [&]() {
        if (import_run_start >= 0 && import_run_last > import_run_start)
            emit(out, lt, import_run_start, import_run_last, "imports");
        import_run_start = -1;
        import_run_last  = -1;
    };

    auto flush_decl_run = [&]() {
        if (decl_run_start >= 0 && decl_run_last > decl_run_start)
            emit(out, lt, decl_run_start, decl_run_last, "declarations");
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

        // ── comments ──────────────────────────────────────────────────────
        if (t.comment_kind != CommentKind::None) {
            if (active_decl_start < 0)
                flush_decl_run();
            size_t offset = t.start;
            int    line   = lt.line_of(offset);

            if (t.comment_kind == CommentKind::Block) {
                flush_comment_run();
                int newlines = (int)std::count(t.text.begin(), t.text.end(), '\n');
                // Only fold block comments that start on their own line.
                // Comment role classification may reference source positioning
                // (per CLAUDE.md exception for comment classification).
                if (newlines > 0 && is_own_line_at_offset(lt.text, offset))
                    emit(out, lt, line, line + newlines, "comment");
            } else if (t.comment_kind == CommentKind::Line) {
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
        if (t.kind == TK::Directive) {
            if (active_decl_start < 0)
                flush_decl_run();
            flush_comment_run();
            // Directives between import statements do not break an import run
            // (mirrors the AST path where directives are trivia, not members).

            int                dir_line = lt.line_of(t.start);

            if (t.directive_kind == slang::syntax::SyntaxKind::IfDefDirective ||
                t.directive_kind == slang::syntax::SyntaxKind::IfNDefDirective) {
                pp_stack.push_back(make_pp_frame(dir_line));
            } else if (t.directive_kind == slang::syntax::SyntaxKind::ElsIfDirective) {
                if (!pp_stack.empty()) {
                    emit(out, lt, pp_stack.back().branch_start_line, dir_line - 1);
                    restore_to_pp_frame(pp_stack.back());
                    pp_stack.back().branch_start_line = dir_line;
                }
            } else if (t.directive_kind == slang::syntax::SyntaxKind::ElseDirective) {
                if (!pp_stack.empty()) {
                    emit(out, lt, pp_stack.back().branch_start_line, dir_line - 1);
                    restore_to_pp_frame(pp_stack.back());
                    pp_stack.back().branch_start_line = dir_line;
                }
            } else if (t.directive_kind == slang::syntax::SyntaxKind::EndIfDirective) {
                if (!pp_stack.empty()) {
                    emit(out, lt, pp_stack.back().branch_start_line, dir_line);
                    restore_to_pp_frame(pp_stack.back());
                    pp_stack.pop_back();
                }
            } else if (t.directive_kind == slang::syntax::SyntaxKind::CellDefineDirective) {
                cell_stack.push_back(dir_line);
            } else if (t.directive_kind == slang::syntax::SyntaxKind::EndCellDefineDirective) {
                if (!cell_stack.empty()) {
                    emit(out, lt, cell_stack.back(), dir_line);
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
        if (t.kind != TK::ImportKeyword)
            flush_import_run();

        const int token_line_number = lt.line_of(t.start);
        if (active_decl_start < 0 && at_statement_start &&
            paren_depth == 0 && brace_depth == 0 &&
            is_decl_start_keyword(t.kind)) {
            active_decl_start = token_line_number;
        } else if (active_decl_start < 0 && at_statement_start &&
                   t.kind != TK::Semicolon) {
            // A non-declaration statement breaks a declaration run.  For
            // example, do not fold declarations across an intervening assign,
            // always block, instance, or assertion.
            flush_decl_run();
        }

        switch (t.kind) {

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
            int line = lt.line_of(t.start);
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
            int line = lt.line_of(t.start);
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
            int end_line = lt.line_of(t.start);
            if (!keyword_region_stack.empty()) {
                emit(out, lt, keyword_region_stack.back(), end_line);
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
            int line = lt.line_of(t.start);
            if (line >= 0) pending_brace_region_start = line;
            pending_control_start = -1;
            break;
        }

        case TK::ConstraintKeyword:
        case TK::CoverPointKeyword:
        case TK::CrossKeyword: {
            int line = lt.line_of(t.start);
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
            int line = lt.line_of(t.start);
            if (line >= 0 && pending_brace_region_start < 0)
                pending_brace_region_start = line;
            pending_control_start = -1;
            break;
        }

        // Bins keywords: wait for = before tracking the brace region
        case TK::BinsKeyword:
        case TK::IllegalBinsKeyword:
        case TK::IgnoreBinsKeyword: {
            int line = lt.line_of(t.start);
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
            int close_line = lt.line_of(t.start);
            if (brace_depth > 0) --brace_depth;
            if (!brace_region_stack.empty() &&
                brace_region_stack.back().outer_depth == brace_depth) {
                emit(out, lt, brace_region_stack.back().start_line, close_line);
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
            int case_line = lt.line_of(t.start);
            if (case_line >= 0) case_stack.push_back(case_line);
            pending_control_start = -1;
            break;
        }

        case TK::EndCaseKeyword: {
            int endcase_line = lt.line_of(t.start);
            if (!case_stack.empty()) {
                emit(out, lt, case_stack.back(), endcase_line);
                case_stack.pop_back();
            }
            pending_control_start = -1;
            break;
        }

        case TK::BeginKeyword: {
            int begin_line = lt.line_of(t.start);
            block_stack.push_back(
                pending_control_start >= 0 ? pending_control_start : begin_line);
            pending_control_start = -1;
            break;
        }

        case TK::EndKeyword: {
            int end_line = lt.line_of(t.start);
            if (!block_stack.empty()) {
                emit(out, lt, block_stack.back(), end_line);
                block_stack.pop_back();
            }
            pending_control_start = -1;
            break;
        }

        // #( introduces a parameter value assignment paren region
        case TK::Hash: {
            if (i + 1 < tokens.size() &&
                tokens[i + 1].kind == TK::OpenParenthesis) {
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
            int open_line = lt.line_of(t.start);
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
            int close_line = lt.line_of(t.start);
            if (!paren_region_stack.empty() &&
                paren_region_stack.back().outer_depth == paren_depth) {
                bool hash_from_header = paren_region_stack.back().hash_from_header;
                emit(out, lt, paren_region_stack.back().start_line, close_line);
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
                   (tokens[next].comment_kind != CommentKind::None ||
                    tokens[next].kind == TK::Unknown))
                ++next;

            if (next < tokens.size() &&
                tokens[next].kind == TK::StringLiteral) {
                // DPI import: skip to closing semicolon, do not track.
                while (i < tokens.size() && tokens[i].kind != TK::Semicolon)
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
                lt.line_of(t.start);
            size_t semi = i + 1;
            while (semi < tokens.size() && tokens[semi].kind != TK::Semicolon)
                ++semi;
            int import_end_line = semi < tokens.size()
                ? lt.line_of(tokens[semi].start)
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
            finish_active_decl(lt.line_of(t.start));
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

// (startLine, endLine, kind) -- the triple normalize_folds() groups folds on.
struct FoldKey {
    int              startLine;
    int              endLine;
    std::string_view kind;

    bool operator==(const FoldKey& other) const {
        return startLine == other.startLine && endLine == other.endLine &&
               kind == other.kind;
    }
};

struct FoldKeyHash {
    size_t operator()(const FoldKey& key) const {
        size_t h = std::hash<std::string_view>{}(key.kind);
        h = h * 31 + (size_t)(unsigned)key.startLine;
        h = h * 31 + (size_t)(unsigned)key.endLine;
        return h;
    }
};

// One hashable value for a (startLine, endLine) pair.  Both are line numbers of
// a file that has been read into memory, so neither comes close to 2^31.
static int64_t span_key(int start_line, int end_line) {
    return ((int64_t)(unsigned)start_line << 32) | (int64_t)(unsigned)end_line;
}

static void normalize_folds(std::vector<FoldingRange>& folds, const LineTable& lt) {
    std::sort(folds.begin(), folds.end(), [](const FoldingRange& a, const FoldingRange& b) {
        return std::tie(a.startLine, a.endLine, a.kind, a.startCharacter, a.endCharacter) <
               std::tie(b.startLine, b.endLine, b.kind, b.startCharacter, b.endCharacter);
    });

    // If the token pass and AST pass both found the same line range, prefer the
    // more precise AST delimiter columns over the token pass's line-indentation
    // columns.  This matters for parameterized module headers where a coarse
    // line-start range on the module line can compete with the enclosing module
    // fold in clients that pick one fold marker per line.
    //
    // Grouping by (startLine, endLine, kind) also drops folds the two passes
    // produced identically, so no separate unique() step is needed.  The group
    // lookup goes through a hash map because the fold list runs to thousands of
    // entries on a large file, and scanning the kept folds for each candidate
    // made this pass quadratic in the file's fold count.
    {
        std::vector<FoldingRange> column_pruned;
        column_pruned.reserve(folds.size());
        // The keys borrow each fold's kind, so this map must not outlive the
        // list it was built from.
        std::unordered_map<FoldKey, size_t, FoldKeyHash> kept_by_key;
        kept_by_key.reserve(folds.size());
        for (const auto& candidate : folds) {
            auto [it, inserted] =
                kept_by_key.try_emplace(FoldKey{candidate.startLine, candidate.endLine,
                                                candidate.kind},
                                        column_pruned.size());
            if (inserted) {
                column_pruned.push_back(candidate);
                continue;
            }
            FoldingRange& existing = column_pruned[it->second];
            if (candidate.startCharacter > existing.startCharacter ||
                (candidate.startCharacter == existing.startCharacter &&
                 candidate.endCharacter > existing.endCharacter))
                existing = candidate;
        }
        folds.swap(column_pruned);
    }

    // Drop redundant "whole header" style folds when the useful split folds
    // already cover the exact same span.  Some clients effectively expose only
    // one fold marker per start line, so keeping (0,11) beside (0,4)+(4,11)
    // can hide the parameter-list fold from users.
    //
    // Index the multi-line region folds once by span and by start line.  The
    // question this pass asks -- "does a region split this one at some interior
    // line" -- then costs two hash lookups per candidate split point instead of
    // a nested walk of the whole fold list, which was cubic in the worst case.
    std::unordered_set<int64_t> region_spans;
    std::unordered_map<int, std::vector<int>> region_ends_by_start;
    region_spans.reserve(folds.size());
    for (const auto& r : folds) {
        if (r.kind != "region" || r.endLine <= r.startLine)
            continue;
        region_spans.insert(span_key(r.startLine, r.endLine));
        region_ends_by_start[r.startLine].push_back(r.endLine);
    }

    std::vector<FoldingRange> no_redundant_headers;
    no_redundant_headers.reserve(folds.size());
    for (const auto& outer : folds) {
        bool redundant_header = false;
        if (outer.kind == "region") {
            if (auto lefts = region_ends_by_start.find(outer.startLine);
                lefts != region_ends_by_start.end()) {
                for (int left_end : lefts->second) {
                    if (left_end >= outer.endLine)
                        continue;
                    if (region_spans.contains(span_key(left_end, outer.endLine))) {
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

    // A parameterized instantiation ends up with two folds that start on the
    // same line: the token scan sees "#(" and emits a paren region ending on
    // the ")" line, while the AST pass emits the whole-statement instance fold.
    //
    //     memory #(          // token region [0,2]   AST instance [0,4]
    //         .W (8)
    //     ) u_mem (
    //         .clk (clk)
    //     );
    //
    // Vim's line-based fold model can only mark one fold start per line, so the
    // shorter parameter-list region becomes the fold za/zc reach first and the
    // instantiation itself never collapses as a whole -- which is the fold the
    // instance pass exists to provide.  Drop the parameter-list region that a
    // longer instance fold starts with.
    std::map<int, int> instance_end_by_start;
    for (const auto& r : folds) {
        if (r.kind != "instance") continue;
        auto [it, inserted] = instance_end_by_start.emplace(r.startLine, r.endLine);
        if (!inserted)
            it->second = std::max(it->second, r.endLine);
    }
    if (!instance_end_by_start.empty()) {
        folds.erase(std::remove_if(folds.begin(), folds.end(),
                                   [&](const FoldingRange& r) {
                                       if (r.kind != "region") return false;
                                       auto it = instance_end_by_start.find(r.startLine);
                                       return it != instance_end_by_start.end() &&
                                              it->second > r.endLine;
                                   }),
                    folds.end());
    }

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
    //
    // A partner can only be a fold that starts on the exact line this one ends
    // on, so index the folds by start line and walk only that line's bucket.
    //
    // This used to scan the whole fold list for every fold, guarded by a count
    // of the folds starting on the candidate line.  That guard removes the walk
    // only when *no* fold starts there, and in indented RTL a fold's end line is
    // very often some other fold's start line -- `end else begin`, a `)` closing
    // one region on the line a `begin` opens another.  So the guard let the walk
    // through constantly and the pass stayed O(folds^2), which is what made
    // whole-file foldingRange super-linear: measured 7.5 / 18.9 / 55.5 / 154 /
    // 903 ms at 3k..50k folds, the last doubling alone costing 5.86x.
    //
    // Buckets are filled in index order, so iterating one visits candidates in
    // the same order the array walk did.  `scan_from` reproduces the old loop's
    // "continue from the match" behaviour, and a fold whose start line is moved
    // below is inserted into its new bucket at the position its index belongs
    // in -- it stays reachable for a later `left`, exactly as it was when every
    // fold was reached by scanning the array.  Its stale entry in the old bucket
    // needs no removal: the `startLine != boundary_line` test below rejects it.
    std::unordered_map<int, std::vector<size_t>> folds_by_start_line;
    folds_by_start_line.reserve(folds.size());
    for (size_t i = 0; i < folds.size(); ++i)
        folds_by_start_line[folds[i].startLine].push_back(i);

    constexpr size_t kNoPartner = (size_t)-1;

    for (size_t li = 0; li < folds.size(); ++li) {
        if (folds[li].kind != "region" || folds[li].startCharacter <= 0 ||
            folds[li].endLine <= folds[li].startLine)
            continue;

        size_t scan_from = 0;
        while (true) {
            // Re-read each time round: a match moves this fold's end line, so
            // the partner test has to be re-asked against the new one.
            const int boundary_line = folds[li].endLine; // the ")(" line
            const auto on_line      = folds_by_start_line.find(boundary_line);
            if (on_line == folds_by_start_line.end())
                break;

            size_t ri = kNoPartner;
            for (size_t candidate : on_line->second) {
                if (candidate < scan_from || candidate == li)
                    continue;
                const auto& right = folds[candidate];
                if (right.kind != "region" || right.startLine != boundary_line ||
                    right.startCharacter >= folds[li].startCharacter ||
                    right.endLine <= right.startLine)
                    continue;
                ri = candidate;
                break;
            }
            if (ri == kNoPartner)
                break;

            auto&     right           = folds[ri];
            const int terminator_line = right.endLine; // the ");" line

            const int left_end    = boundary_line - 1;
            const int right_start = boundary_line + 1;
            const int right_end   = terminator_line - 1;

            // Keep only meaningful multi-line folds.  If a tiny header has no
            // interior lines after removing delimiter lines, the invalid range
            // is pruned below instead of exposing a misleading one-line fold.
            folds[li].endLine      = left_end;
            folds[li].endCharacter = lt.line_length(left_end);

            right.startLine      = right_start;
            right.startCharacter = lt.first_non_space_column(right_start);
            right.endLine        = right_end;
            right.endCharacter   = lt.line_length(right_end);

            auto& moved_bucket = folds_by_start_line[right_start];
            moved_bucket.insert(
                std::lower_bound(moved_bucket.begin(), moved_bucket.end(), ri), ri);

            scan_from = ri + 1;
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
    //
    // Ordering the declaration folds by start ascending, end descending makes
    // "is any other declaration fold wide enough to contain this one" a running
    // maximum over the ones already seen: every earlier entry starts no later,
    // so it contains this one exactly when its end reaches at least as far.
    // Comparing every declaration fold against every other one was the third
    // quadratic pass here.  Exact duplicates cannot reach this point -- the
    // grouping at the top of this function collapsed them -- so an equal end
    // from an equal start is always a genuinely wider fold.
    std::vector<size_t> declarations;
    for (size_t i = 0; i < folds.size(); ++i)
        if (folds[i].kind == "declarations")
            declarations.push_back(i);

    std::vector<char> contained_in_declaration(folds.size(), 0);
    if (declarations.size() > 1) {
        std::sort(declarations.begin(), declarations.end(), [&](size_t a, size_t b) {
            if (folds[a].startLine != folds[b].startLine)
                return folds[a].startLine < folds[b].startLine;
            return folds[a].endLine > folds[b].endLine;
        });
        int widest_end = std::numeric_limits<int>::min();
        for (size_t i : declarations) {
            if (widest_end >= folds[i].endLine)
                contained_in_declaration[i] = 1;
            widest_end = std::max(widest_end, folds[i].endLine);
        }
    }

    std::vector<FoldingRange> pruned;
    pruned.reserve(folds.size());
    for (size_t i = 0; i < folds.size(); ++i)
        if (!contained_in_declaration[i])
            pruned.push_back(folds[i]);
    folds.swap(pruned);
}

} // namespace

namespace {

/// The folds the token scan finds, before normalization.
///
/// lex_fold_tokens() lexes without preprocessing, so tokens from both active and
/// inactive preprocessor branches appear in the stream.  It needs only the
/// document text, which is what lets a buffer whose parse has not landed still
/// be answered.
///
/// Takes the line table rather than building one: both halves of a request need
/// it, and it is a scan of the whole document.  Building one per half meant
/// every keystroke walked the file an extra time for a table it already had.
std::vector<FoldingRange> token_folds_unnormalized(const std::string& text, const LineTable& lt) {
    const std::vector<FoldToken> tokens = lex_fold_tokens(text);
    std::vector<FoldingRange>    out;
    collect_token_folds(tokens, lt, out);
    return out;
}

std::vector<FoldingRange> token_folds(const std::string& text) {
    LineTable lt{text};
    auto      out = token_folds_unnormalized(text, lt);
    normalize_folds(out, lt);
    return out;
}

} // namespace

// ── public API ────────────────────────────────────────────────────────────

std::shared_ptr<const std::vector<FoldingRange>>
provide_folding_range_shared(const Analyzer& analyzer, const FoldingRangeRequestParams& params) {
    auto state = analyzer.get_state(params.textDocument.uri.raw_uri_);
    if (!state)
        return nullptr;

    // Folds are derived from the token scan and nothing else, so this answer
    // does not depend on the parse the document's last notification started.
    // There is no tree to wait for and no reparse window to bridge: every
    // request is served from the text the client last sent.  That is what keeps
    // the editor's per-keystroke foldingRange off the AST entirely.
    //
    // It is also what makes the answer a property of the snapshot rather than
    // of the request, so the whole queue of fold requests that piles up behind
    // a fast burst -- key repeat, a paste, a macro -- shares one computation
    // once the edits stop arriving, instead of repeating it once per request.
    if (auto cached = state->folding_ranges())
        return cached;

    state->set_folding_ranges(
        std::make_shared<const std::vector<FoldingRange>>(token_folds(state->text)));
    // Read back rather than returned directly: the slot keeps its first writer,
    // so a request that raced another one hands back the vector everybody else
    // is holding instead of an equal copy of its own.
    return state->folding_ranges();
}

std::vector<FoldingRange> provide_folding_range(const Analyzer& analyzer,
                                                const FoldingRangeRequestParams& params) {
    auto folds = provide_folding_range_shared(analyzer, params);
    return folds ? *folds : std::vector<FoldingRange>{};
}
