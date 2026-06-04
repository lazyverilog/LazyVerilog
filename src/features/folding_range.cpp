#include "folding_range.hpp"
#include "document_state.hpp"
#include "formatter_lexer.hpp"
#include "formatter_token.hpp"

#include <algorithm>
#include <cctype>
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

static std::pair<size_t, size_t> line_bounds(std::string_view text, int line) {
    if (line < 0) return {0, 0};
    size_t start = 0;
    for (int current = 0; current < line && start < text.size(); ++current) {
        size_t newline = text.find('\n', start);
        if (newline == std::string_view::npos) return {text.size(), text.size()};
        start = newline + 1;
    }
    size_t end = text.find('\n', start);
    if (end == std::string_view::npos) end = text.size();
    return {start, end};
}

static int first_non_space_column(std::string_view text, int line) {
    auto [start, end] = line_bounds(text, line);
    for (size_t i = start; i < end; ++i) {
        unsigned char ch = static_cast<unsigned char>(text[i]);
        if (!std::isspace(ch)) return static_cast<int>(i - start);
    }
    return 0;
}

static int line_length(std::string_view text, int line) {
    auto [start, end] = line_bounds(text, line);
    return static_cast<int>(end - start);
}

// ── AST path helpers (used only by IfElseChainVisitor) ───────────────────

static int token_line(const SourceManager& sm, const Token& tok) {
    if (!tok || !tok.location().valid()) return -1;
    return (int)sm.getLineNumber(tok.location()) - 1;
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
    auto text = sm.getSourceText(buffer);
    FoldingRange r;
    r.startLine      = start;
    r.endLine        = end;
    r.startCharacter = first_non_space_column(text, start);
    r.endCharacter   = line_length(text, end);
    r.kind           = kind;
    out.push_back(r);
}

static void emit_node(std::vector<FoldingRange>& out, const SourceManager& sm,
                      const SyntaxNode& node, const std::string& kind = "region") {
    Token first = node.getFirstToken();
    if (!first || !first.location().valid()) return;
    emit(out, sm, first.location().buffer(), first_line(sm, node), last_line(sm, node), kind);
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
    std::vector<FoldingRange>& out;

    IfElseChainVisitor(const SourceManager& sm, std::vector<FoldingRange>& out)
        : sm(sm), out(out) {}

    void handle(const ConditionalStatementSyntax& node) {
        emit_node(out, sm, node);
        visitDefault(node);
    }

    void handle(const IfGenerateSyntax& node) {
        int node_start = first_line(sm, node);
        int node_end   = last_line(sm, node);
        if (node.elseClause) {
            int else_line = token_line(sm, node.elseClause->elseKeyword);
            emit(out, sm, node.getFirstToken().location().buffer(),
                 node_start, else_line - 1);
            emit(out, sm, node.getFirstToken().location().buffer(),
                 else_line, node_end);
        } else {
            emit(out, sm, node.getFirstToken().location().buffer(),
                 node_start, node_end);
        }
        visitDefault(node);
    }
};

// ── token path helpers ────────────────────────────────────────────────────

// Convert a byte offset in source_text to a 0-based line number.
static int line_of(std::string_view text, size_t offset) {
    int line = 0;
    size_t lim = std::min(offset, text.size());
    for (size_t i = 0; i < lim; ++i) {
        if (text[i] == '\n') ++line;
    }
    return line;
}

// Emit a fold using source_text for character-column computation.
// Used exclusively by collect_token_folds(); the AST post-pass uses emit().
static void emit_token_fold(std::vector<FoldingRange>& out, std::string_view source_text,
                            int start, int end, const std::string& kind = "region") {
    if (start < 0 || end < 0 || start >= end) return;
    FoldingRange r;
    r.startLine      = start;
    r.endLine        = end;
    r.startCharacter = first_non_space_column(source_text, start);
    r.endCharacter   = line_length(source_text, end);
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
                                std::string_view source_text,
                                std::vector<FoldingRange>& out) {
    using TK = TokenKind;

    struct BraceRegion { int start_line{-1}; int outer_depth{0}; };
    struct ParenRegion  { int start_line{-1}; int outer_depth{0}; };

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
    int  brace_depth{0};
    int  paren_depth{0};

    int comment_run_start{-1};
    int comment_run_last{-1};
    int import_run_start{-1};
    int import_run_last{-1};

    auto flush_comment_run = [&]() {
        if (comment_run_start >= 0 && comment_run_last > comment_run_start)
            emit_token_fold(out, source_text, comment_run_start, comment_run_last, "comment");
        comment_run_start = -1;
        comment_run_last  = -1;
    };

    auto flush_import_run = [&]() {
        if (import_run_start >= 0 && import_run_last > import_run_start)
            emit_token_fold(out, source_text, import_run_start, import_run_last, "imports");
        import_run_start = -1;
        import_run_last  = -1;
    };

    for (size_t i = 0; i < tokens.size(); ++i) {
        const auto& t = tokens[i];

        // Skip verbatim passthrough bodies from format-off/on regions.
        if (t.lex->is_disabled_region_body) continue;

        // ── comments ──────────────────────────────────────────────────────
        if (t.lex->is_comment) {
            size_t offset = t.lex->range.start().offset();
            int    line   = line_of(source_text, offset);

            if (t.lex->comment_kind == svfmt::CommentLexemeKind::Block) {
                flush_comment_run();
                int newlines = (int)std::count(t.lex->text.begin(), t.lex->text.end(), '\n');
                // Only fold block comments that start on their own line.
                // Comment role classification may reference source positioning
                // (per CLAUDE.md exception for comment classification).
                if (newlines > 0 && is_own_line_at_offset(source_text, offset))
                    emit_token_fold(out, source_text, line, line + newlines, "comment");
            } else if (t.lex->comment_kind == svfmt::CommentLexemeKind::Line) {
                if (!is_own_line_at_offset(source_text, offset)) {
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
        if (t.lex->is_directive) {
            flush_comment_run();
            // Directives between import statements do not break an import run
            // (mirrors the AST path where directives are trivia, not members).

            const std::string& lt       = t.lex->lower_text;
            int                dir_line = line_of(source_text, t.lex->range.start().offset());

            auto sw = [&](const char* prefix) {
                return lt.rfind(prefix, 0) == 0;
            };

            if (sw("`ifdef") || sw("`ifndef")) {
                pp_stack.push_back(dir_line);
            } else if (sw("`elsif")) {
                if (!pp_stack.empty()) {
                    emit_token_fold(out, source_text, pp_stack.back(), dir_line - 1);
                    pp_stack.back() = dir_line;
                }
            } else if (sw("`else") && !sw("`elsif")) {
                if (!pp_stack.empty()) {
                    emit_token_fold(out, source_text, pp_stack.back(), dir_line - 1);
                    pp_stack.back() = dir_line;
                }
            } else if (sw("`endif")) {
                if (!pp_stack.empty()) {
                    emit_token_fold(out, source_text, pp_stack.back(), dir_line);
                    pp_stack.pop_back();
                }
            } else if (sw("`celldefine")) {
                cell_stack.push_back(dir_line);
            } else if (sw("`endcelldefine")) {
                if (!cell_stack.empty()) {
                    emit_token_fold(out, source_text, cell_stack.back(), dir_line);
                    cell_stack.pop_back();
                }
            }

            pending_control_start = -1;
            pending_paren_region  = false;
            continue;
        }

        // ── all other tokens ───────────────────────────────────────────────
        // Any non-comment, non-directive code token flushes the comment run.
        // Any such token that is not ImportKeyword also flushes the import run
        // (matches AST behavior where non-import members break import groups).
        flush_comment_run();
        if (t.lex->kind != TK::ImportKeyword)
            flush_import_run();

        switch (t.lex->kind) {

        // Control keywords: mark pending start for begin attribution
        case TK::AlwaysKeyword:
        case TK::AlwaysCombKeyword:
        case TK::AlwaysFFKeyword:
        case TK::AlwaysLatchKeyword:
        case TK::InitialKeyword:
        case TK::FinalKeyword:
        case TK::IfKeyword:
        case TK::ElseKeyword:
        case TK::ForKeyword:
        case TK::ForeachKeyword:
        case TK::ForeverKeyword:
        case TK::WhileKeyword:
        case TK::RepeatKeyword: {
            int line = line_of(source_text, t.lex->range.start().offset());
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
            int line = line_of(source_text, t.lex->range.start().offset());
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
            int end_line = line_of(source_text, t.lex->range.start().offset());
            if (!keyword_region_stack.empty()) {
                emit_token_fold(out, source_text, keyword_region_stack.back(), end_line);
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
            int line = line_of(source_text, t.lex->range.start().offset());
            if (line >= 0) pending_brace_region_start = line;
            pending_control_start = -1;
            break;
        }

        case TK::ConstraintKeyword:
        case TK::CoverPointKeyword:
        case TK::CrossKeyword: {
            int line = line_of(source_text, t.lex->range.start().offset());
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
            int line = line_of(source_text, t.lex->range.start().offset());
            if (line >= 0 && pending_brace_region_start < 0)
                pending_brace_region_start = line;
            pending_control_start = -1;
            break;
        }

        // Bins keywords: wait for = before tracking the brace region
        case TK::BinsKeyword:
        case TK::IllegalBinsKeyword:
        case TK::IgnoreBinsKeyword: {
            int line = line_of(source_text, t.lex->range.start().offset());
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
            int close_line = line_of(source_text, t.lex->range.start().offset());
            if (brace_depth > 0) --brace_depth;
            if (!brace_region_stack.empty() &&
                brace_region_stack.back().outer_depth == brace_depth) {
                emit_token_fold(out, source_text,
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
            int case_line = line_of(source_text, t.lex->range.start().offset());
            if (case_line >= 0) case_stack.push_back(case_line);
            pending_control_start = -1;
            break;
        }

        case TK::EndCaseKeyword: {
            int endcase_line = line_of(source_text, t.lex->range.start().offset());
            if (!case_stack.empty()) {
                emit_token_fold(out, source_text, case_stack.back(), endcase_line);
                case_stack.pop_back();
            }
            pending_control_start = -1;
            break;
        }

        case TK::BeginKeyword: {
            int begin_line = line_of(source_text, t.lex->range.start().offset());
            block_stack.push_back(
                pending_control_start >= 0 ? pending_control_start : begin_line);
            pending_control_start = -1;
            break;
        }

        case TK::EndKeyword: {
            int end_line = line_of(source_text, t.lex->range.start().offset());
            if (!block_stack.empty()) {
                emit_token_fold(out, source_text, block_stack.back(), end_line);
                block_stack.pop_back();
            }
            pending_control_start = -1;
            break;
        }

        // #( introduces a parameter value assignment paren region
        case TK::Hash: {
            if (i + 1 < tokens.size() &&
                tokens[i + 1].lex->kind == TK::OpenParenthesis)
                pending_paren_region = true;
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
            int open_line = line_of(source_text, t.lex->range.start().offset());
            if (pending_paren_region) {
                paren_region_stack.push_back({open_line, paren_depth});
                pending_paren_region = false;
            }
            ++paren_depth;
            pending_control_start = -1;
            break;
        }

        case TK::CloseParenthesis: {
            if (paren_depth > 0) --paren_depth;
            int close_line = line_of(source_text, t.lex->range.start().offset());
            if (!paren_region_stack.empty() &&
                paren_region_stack.back().outer_depth == paren_depth) {
                emit_token_fold(out, source_text,
                                paren_region_stack.back().start_line, close_line);
                paren_region_stack.pop_back();
            }
            pending_control_start = -1;
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
                   (tokens[next].lex->is_comment ||
                    tokens[next].lex->kind == TK::Unknown ||
                    tokens[next].lex->is_disabled_region_body))
                ++next;

            if (next < tokens.size() &&
                tokens[next].lex->kind == TK::StringLiteral) {
                // DPI import: skip to closing semicolon, do not track.
                while (i < tokens.size() && tokens[i].lex->kind != TK::Semicolon)
                    ++i;
                break;
            }

            // Package import: extent is from ImportKeyword to its Semicolon.
            int    import_start_line =
                line_of(source_text, t.lex->range.start().offset());
            size_t semi = i + 1;
            while (semi < tokens.size() && tokens[semi].lex->kind != TK::Semicolon)
                ++semi;
            int import_end_line = semi < tokens.size()
                ? line_of(source_text, tokens[semi].lex->range.start().offset())
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
            break;
        }

        case TK::Semicolon:
            pending_control_start      = -1;
            pending_brace_region_start = -1;
            pending_bins_start         = -1;
            pending_bins_equals        = false;
            pending_paren_region       = false;
            break;

        default:
            pending_control_start = -1;
            break;
        }
    }

    flush_comment_run();
    flush_import_run();
}

// ── normalization ─────────────────────────────────────────────────────────

static void normalize_folds(std::vector<FoldingRange>& folds) {
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
    collect_token_folds(tokens, state->text, out);

    // AST post-pass: if/else chain linking only.
    // ConditionalStatementSyntax and IfGenerateSyntax require AST precision
    // to link chained branches; token-only detection cannot reliably do this.
    if (state->source_manager) {
        IfElseChainVisitor v{*state->source_manager, out};
        state->tree->root().visit(v);
    }

    normalize_folds(out);
    return out;
}
