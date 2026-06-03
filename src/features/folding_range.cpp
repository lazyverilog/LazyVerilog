#include "folding_range.hpp"
#include "document_state.hpp"

#include <algorithm>
#include <cctype>
#include <string_view>
#include <tuple>
#include <unordered_map>
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

// ── helpers ────────────────────────────────────────────────────────────────

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

static std::pair<size_t, size_t> line_bounds(std::string_view text, int line) {
    // LSP folding ranges are line-oriented, but startCharacter / endCharacter
    // are still real positions, not placeholders.  Compute bounds directly from
    // the source buffer so clients that honor these fields do not interpret every
    // fold as ending at column 0 of the closing line.
    //
    // The returned pair is [line_start_offset, line_end_offset), where line_end
    // points at the newline byte or text.size().  The helper accepts a zero-based
    // line number to match LSP and all folding code in this file.
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

// ── fold visitor ──────────────────────────────────────────────────────────

struct FoldVisitor : public SyntaxVisitor<FoldVisitor> {
    const SourceManager&       sm;
    std::vector<FoldingRange>& out;
    std::vector<FoldingRange>  directive_folds;

    struct PreprocessorBranch {
        int start_line{-1};
        BufferID buffer{};
    };

    // preprocessor stack: current branch start for nested `ifdef / `elsif / `else blocks
    std::vector<PreprocessorBranch> pp_stack;

    struct OpenLine {
        int start_line{-1};
        BufferID buffer{};
    };

    // celldefine stack: line / buffer of the opening `celldefine
    std::vector<OpenLine> cell_stack;

    // consecutive line-comment run
    int comment_run_start{-1};
    int comment_run_last{-1};
    BufferID comment_run_buffer{};

    // cache: line number → byte offset of line start; cleared on buffer change
    mutable BufferID                        line_start_cache_buf_{};
    mutable std::unordered_map<int, size_t> line_start_cache_;

    FoldVisitor(const SourceManager& sm, std::vector<FoldingRange>& out)
        : sm(sm), out(out) {}

    void flush_comment_run() {
        if (comment_run_start >= 0 && comment_run_last > comment_run_start)
            emit(out, sm, comment_run_buffer, comment_run_start, comment_run_last, "comment");
        comment_run_start = -1;
        comment_run_last  = -1;
        comment_run_buffer = {};
    }

    void emit_directive(BufferID buffer, int start, int end) {
        emit(directive_folds, sm, buffer, start, end, "region");
    }

    // ── top-level declarations ────────────────────────────────────────────

    // module / macromodule / interface / program / package (all use same type)
    void handle(const ModuleDeclarationSyntax& node) {
        emit_node(out, sm, node);
        visitDefault(node);
    }

    void handle(const ClassDeclarationSyntax& node) {
        emit_node(out, sm, node);
        visitDefault(node);
    }

    void handle(const CheckerDeclarationSyntax& node) {
        emit_node(out, sm, node);
        visitDefault(node);
    }

    // primitive / UDP
    void handle(const UdpDeclarationSyntax& node) {
        emit_node(out, sm, node);
        visitDefault(node);
    }

    void handle(const ConfigDeclarationSyntax& node) {
        emit_node(out, sm, node);
        visitDefault(node);
    }

    void handle(const ClockingDeclarationSyntax& node) {
        emit_node(out, sm, node);
        visitDefault(node);
    }

    // function / task (TaskDeclaration maps to FunctionDeclarationSyntax)
    void handle(const FunctionDeclarationSyntax& node) {
        emit_node(out, sm, node);
        visitDefault(node);
    }

    // ── statement blocks ──────────────────────────────────────────────────

    // begin/end (SequentialBlockStatement) + fork/join* (ParallelBlockStatement)
    void handle(const BlockStatementSyntax& node) {
        emit_node(out, sm, node);
        visitDefault(node);
    }

    // if / else if / else — fold the whole chain as one region
    void handle(const ConditionalStatementSyntax& node) {
        emit_node(out, sm, node);
        visitDefault(node);
    }

    // case / casex / casez / endcase
    void handle(const CaseStatementSyntax& node) {
        emit_node(out, sm, node);
        visitDefault(node);
    }

    // randcase / endcase
    void handle(const RandCaseStatementSyntax& node) {
        emit_node(out, sm, node);
        visitDefault(node);
    }

    // individual multi-line case items
    void handle(const StandardCaseItemSyntax& node) {
        emit_node(out, sm, node);
        visitDefault(node);
    }

    // ── verification constructs ───────────────────────────────────────────

    void handle(const ConstraintDeclarationSyntax& node) {
        emit_node(out, sm, node);
        visitDefault(node);
    }

    void handle(const CovergroupDeclarationSyntax& node) {
        emit_node(out, sm, node);
        visitDefault(node);
    }

    void handle(const PropertyDeclarationSyntax& node) {
        emit_node(out, sm, node);
        visitDefault(node);
    }

    void handle(const SequenceDeclarationSyntax& node) {
        emit_node(out, sm, node);
        visitDefault(node);
    }

    void handle(const SpecifyBlockSyntax& node) {
        emit_node(out, sm, node);
        visitDefault(node);
    }

    // ── generate constructs ───────────────────────────────────────────────

    void handle(const GenerateRegionSyntax& node) {
        emit_node(out, sm, node);
        visitDefault(node);
    }

    void handle(const LoopGenerateSyntax& node) {
        emit_node(out, sm, node);
        visitDefault(node);
    }

    void handle(const IfGenerateSyntax& node) {
        int node_start = first_line(sm, node);
        int node_end   = last_line(sm, node);
        if (node.elseClause) {
            int else_line = token_line(sm, node.elseClause->elseKeyword);
            // fold the if arm
            emit(out, sm, node.getFirstToken().location().buffer(), node_start, else_line - 1);
            // fold the else arm
            emit(out, sm, node.getFirstToken().location().buffer(), else_line, node_end);
        } else {
            emit(out, sm, node.getFirstToken().location().buffer(), node_start, node_end);
        }
        visitDefault(node);
    }

    void handle(const CaseGenerateSyntax& node) {
        emit_node(out, sm, node);
        visitDefault(node);
    }

    void handle(const GenerateBlockSyntax& node) {
        emit_node(out, sm, node);
        visitDefault(node);
    }

    // ── port / parameter lists ────────────────────────────────────────────

    void handle(const AnsiPortListSyntax& node) {
        emit_node(out, sm, node);
        visitDefault(node);
    }

    void handle(const ParameterPortListSyntax& node) {
        emit_node(out, sm, node);
        visitDefault(node);
    }

    // instance parameter override #(...)
    void handle(const ParameterValueAssignmentSyntax& node) {
        emit_node(out, sm, node);
        visitDefault(node);
    }

    // instance connection list (...)
    void handle(const HierarchicalInstanceSyntax& node) {
        emit_node(out, sm, node);
        visitDefault(node);
    }

    // multi-line argument lists
    void handle(const ArgumentListSyntax& node) {
        emit_node(out, sm, node);
        visitDefault(node);
    }

    // ── token trivia (comments) ───────────────────────────────────────────

    void visitToken(Token token) {
        if (!token || !token.location().valid()) return;

        auto    buffer      = token.location().buffer();
        size_t  tok_offset  = token.location().offset();

        // derive trivia start offset: trivia comes immediately before the token
        size_t trivia_total = 0;
        for (const auto& t : token.trivia())
            trivia_total += t.getRawText().size();
        // If tok_offset < trivia_total the layout invariant is violated (e.g.
        // synthetic token with an impossible offset).  Skip rather than clamping
        // to 0 and emitting folds attributed to the wrong line.
        if (tok_offset < trivia_total) return;
        size_t pos = tok_offset - trivia_total;

        for (const auto& t : token.trivia()) {
            process_trivia(t, pos, buffer);
            pos += t.getRawText().size();
        }
    }

    void process_trivia(const Trivia& t, size_t offset, BufferID buffer) {
        using TV = TriviaKind;

        if (t.kind == TV::Directive) {
            //
            // Preprocessor directives are represented by slang as structured
            // directive trivia attached to regular tokens, not necessarily as
            // children visited by SyntaxVisitor::visitDefault().  Use the directive
            // SyntaxKind here instead of scanning source lines for strings such as
            // "`ifdef" or "`endif".
            flush_comment_run();
            if (auto* syntax = t.syntax())
                process_directive_syntax(*syntax);
        } else if (t.kind == TV::BlockComment) {
            flush_comment_run();
            auto raw       = t.getRawText();
            int  start     = line_at(buffer, offset);
            int  newlines  = (int)std::count(raw.begin(), raw.end(), '\n');
            //
            // Only fold block comments that start on their own line.  A multi-line
            // block comment can legally appear after code:
            //
            //     assign a = b; /* explanation
            //                      continues */
            //
            // Folding from that line would hide real RTL, which is surprising and
            // different from how editors normally treat comment folds.  Own-line
            // detection is intentionally source-position based; unlike syntax folds,
            // comment role classification has to know whether text before the comment
            // on the same physical line is whitespace or code.
            if (start >= 0 && newlines > 0 && is_own_line_trivia(buffer, offset))
                emit(out, sm, buffer, start, start + newlines, "comment");
        } else if (t.kind == TV::LineComment) {
            int line = line_at(buffer, offset);
            if (line < 0) return;
            //
            // Do not let trailing comments join an own-line comment run.  Otherwise
            // this RTL:
            //
            //     assign a = b; // trailing
            //     // own-line
            //
            // would produce comment fold [assign-line, own-comment-line], hiding the
            // assignment when the user folds comments.
            if (!is_own_line_trivia(buffer, offset)) {
                flush_comment_run();
                return;
            }
            if (comment_run_start < 0) {
                comment_run_start = line;
                comment_run_last  = line;
                comment_run_buffer = buffer;
            } else if (line == comment_run_last + 1) {
                comment_run_last = line;
            } else {
                flush_comment_run();
                comment_run_start = line;
                comment_run_last  = line;
                comment_run_buffer = buffer;
            }
        } else if (t.kind != TV::Whitespace && t.kind != TV::EndOfLine) {
            // directive or other non-whitespace trivia breaks a comment run
            flush_comment_run();
        }
    }

    void process_directive_syntax(const SyntaxNode& syntax) {
        if (const auto* node = syntax.as_if<ConditionalBranchDirectiveSyntax>()) {
            process_conditional_branch_directive(*node);
        } else if (const auto* node = syntax.as_if<UnconditionalBranchDirectiveSyntax>()) {
            process_unconditional_branch_directive(*node);
        } else if (const auto* node = syntax.as_if<SimpleDirectiveSyntax>()) {
            process_simple_directive(*node);
        }
    }

    void process_conditional_branch_directive(const ConditionalBranchDirectiveSyntax& node) {
        collect_disabled_token_folds(node.disabledTokens);

        int line = token_line(sm, node.directive);
        BufferID buffer = node.directive.location().buffer();
        if (node.kind == SyntaxKind::IfDefDirective ||
            node.kind == SyntaxKind::IfNDefDirective) {
            pp_stack.push_back({line, buffer});
        } else if (node.kind == SyntaxKind::ElsIfDirective) {
            if (!pp_stack.empty()) {
                emit_directive(pp_stack.back().buffer, pp_stack.back().start_line, line - 1);
                pp_stack.back() = {line, buffer};
            }
        }
    }

    void process_unconditional_branch_directive(const UnconditionalBranchDirectiveSyntax& node) {
        collect_disabled_token_folds(node.disabledTokens);

        int line = token_line(sm, node.directive);
        BufferID buffer = node.directive.location().buffer();
        if (node.kind == SyntaxKind::ElseDirective) {
            if (!pp_stack.empty()) {
                emit_directive(pp_stack.back().buffer, pp_stack.back().start_line, line - 1);
                pp_stack.back() = {line, buffer};
            }
        } else if (node.kind == SyntaxKind::EndIfDirective) {
            if (!pp_stack.empty()) {
                PreprocessorBranch branch = pp_stack.back();
                // Fold only the current preprocessor branch body:
                //
                //     `ifdef FOO      <-- fold starts here
                //         ...
                //     `else           <-- first branch folds to the line before this
                //         ...
                //     `endif          <-- final branch folds through this line
                //
                // The first branch must not hide the `else directive.  The final
                // branch should include the closing `endif so folding on `else,
                // the last `elsif, or an `ifdef with no alternate branch hides the
                // whole branch including its terminator.
                emit_directive(branch.buffer, branch.start_line, line);
                pp_stack.pop_back();
            }
        }
    }

    void collect_disabled_token_folds(const TokenList& tokens) {
        //
        // Inactive preprocessor branches are not represented as normal AST nodes,
        // so an `ifdef branch that contains:
        //
        //     always_ff (...) begin
        //         if (...) begin
        //             ...
        //         end
        //     end
        //
        // would otherwise only have the coarse preprocessor branch fold.  That is
        // exactly the bad UX where "za" inside the if/always block closes the whole
        // `ifdef.  Use token kinds from slang's disabled token list to recover the
        // most important local block folds without comparing raw source strings.
        //
        // This intentionally handles only token-delimited regions in disabled
        // branches.  Active branches continue to use the real AST visitor.  This is
        // still deliberately a lightweight, token-kind-only recovery path rather
        // than a second parser for disabled SystemVerilog; do not add raw source
        // string searches or regexes here.
        struct BraceRegion {
            int start_line{-1};
            int outer_depth{0};
        };

        std::vector<int> block_stack;
        std::vector<int> case_stack;
        std::vector<int> keyword_region_stack;
        std::vector<BraceRegion> brace_region_stack;
        int              pending_control_start{-1};
        int              pending_brace_region_start{-1};
        int              pending_bins_start{-1};
        int              brace_depth{0};
        bool             pending_bins_equals{false};

        auto mark_control_start = [&](const Token& token) {
            int line = token_line(sm, token);
            if (line >= 0)
                pending_control_start = line;
        };

        auto push_keyword_region = [&](const Token& token) {
            int line = token_line(sm, token);
            if (line >= 0)
                keyword_region_stack.push_back(line);
            pending_control_start = -1;
        };

        auto pop_keyword_region = [&](const Token& token) {
            int end_line = token_line(sm, token);
            if (!keyword_region_stack.empty()) {
                emit(directive_folds, sm, token.location().buffer(),
                     keyword_region_stack.back(), end_line, "region");
                keyword_region_stack.pop_back();
            }
            pending_control_start = -1;
        };

        auto mark_pending_brace_region = [&](const Token& token) {
            int line = token_line(sm, token);
            if (line >= 0)
                pending_brace_region_start = line;
            pending_control_start = -1;
        };

        auto mark_pending_bins_region = [&](const Token& token) {
            int line = token_line(sm, token);
            if (line >= 0) {
                pending_bins_start  = line;
                pending_bins_equals = false;
            }
            pending_control_start = -1;
        };

        for (const auto& token : tokens) {
            switch (token.kind) {
            case TokenKind::AlwaysKeyword:
            case TokenKind::AlwaysCombKeyword:
            case TokenKind::AlwaysFFKeyword:
            case TokenKind::AlwaysLatchKeyword:
            case TokenKind::InitialKeyword:
            case TokenKind::FinalKeyword:
            case TokenKind::IfKeyword:
            case TokenKind::ElseKeyword:
            case TokenKind::ForKeyword:
            case TokenKind::ForeachKeyword:
            case TokenKind::ForeverKeyword:
            case TokenKind::WhileKeyword:
            case TokenKind::RepeatKeyword:
                mark_control_start(token);
                break;

            case TokenKind::ForkKeyword:
            case TokenKind::GenerateKeyword:
            case TokenKind::FunctionKeyword:
            case TokenKind::TaskKeyword:
            case TokenKind::ClassKeyword:
            case TokenKind::CoverGroupKeyword:
            case TokenKind::PropertyKeyword:
            case TokenKind::SequenceKeyword:
            case TokenKind::CheckerKeyword:
            case TokenKind::PrimitiveKeyword:
            case TokenKind::ConfigKeyword:
            case TokenKind::SpecifyKeyword:
            case TokenKind::PackageKeyword:
            case TokenKind::InterfaceKeyword:
            case TokenKind::ProgramKeyword:
            case TokenKind::ModuleKeyword:
            case TokenKind::MacromoduleKeyword:
                push_keyword_region(token);
                break;

            case TokenKind::JoinKeyword:
            case TokenKind::JoinAnyKeyword:
            case TokenKind::JoinNoneKeyword:
            case TokenKind::EndGenerateKeyword:
            case TokenKind::EndFunctionKeyword:
            case TokenKind::EndTaskKeyword:
            case TokenKind::EndClassKeyword:
            case TokenKind::EndGroupKeyword:
            case TokenKind::EndPropertyKeyword:
            case TokenKind::EndSequenceKeyword:
            case TokenKind::EndCheckerKeyword:
            case TokenKind::EndPrimitiveKeyword:
            case TokenKind::EndConfigKeyword:
            case TokenKind::EndSpecifyKeyword:
            case TokenKind::EndPackageKeyword:
            case TokenKind::EndInterfaceKeyword:
            case TokenKind::EndProgramKeyword:
            case TokenKind::EndModuleKeyword:
                pop_keyword_region(token);
                break;

            case TokenKind::ConstraintKeyword:
            case TokenKind::CoverPointKeyword:
            case TokenKind::CrossKeyword:
                mark_pending_brace_region(token);
                break;

            case TokenKind::BinsKeyword:
            case TokenKind::IllegalBinsKeyword:
            case TokenKind::IgnoreBinsKeyword:
                mark_pending_bins_region(token);
                break;

            case TokenKind::Equals:
                if (pending_bins_start >= 0)
                    pending_bins_equals = true;
                break;

            case TokenKind::OpenBrace:
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

            case TokenKind::CloseBrace: {
                int close_line = token_line(sm, token);
                if (brace_depth > 0)
                    --brace_depth;
                if (!brace_region_stack.empty() &&
                    brace_region_stack.back().outer_depth == brace_depth) {
                    emit(directive_folds, sm, token.location().buffer(),
                         brace_region_stack.back().start_line, close_line, "region");
                    brace_region_stack.pop_back();
                }
                pending_control_start = -1;
                break;
            }

            case TokenKind::ApostropheOpenBrace:
                ++brace_depth;
                pending_control_start = -1;
                break;

            case TokenKind::CaseKeyword:
            case TokenKind::CaseXKeyword:
            case TokenKind::CaseZKeyword:
            case TokenKind::RandCaseKeyword: {
                int case_line = token_line(sm, token);
                if (case_line >= 0)
                    case_stack.push_back(case_line);
                pending_control_start = -1;
                break;
            }

            case TokenKind::EndCaseKeyword: {
                int endcase_line = token_line(sm, token);
                if (!case_stack.empty()) {
                    emit(directive_folds, sm, token.location().buffer(), case_stack.back(),
                         endcase_line, "region");
                    case_stack.pop_back();
                }
                pending_control_start = -1;
                break;
            }

            case TokenKind::BeginKeyword: {
                int begin_line = token_line(sm, token);
                block_stack.push_back(pending_control_start >= 0 ? pending_control_start
                                                                 : begin_line);
                pending_control_start = -1;
                break;
            }

            case TokenKind::EndKeyword: {
                int end_line = token_line(sm, token);
                if (!block_stack.empty()) {
                    emit(directive_folds, sm, token.location().buffer(), block_stack.back(),
                         end_line, "region");
                    block_stack.pop_back();
                }
                pending_control_start = -1;
                break;
            }

            case TokenKind::Semicolon:
                // A semicolon after a control keyword means no begin follows
                // (e.g. `if (cond) a <= 1;`).  Clear pending so the stale
                // start line is not attributed to a later unrelated begin.
                pending_control_start = -1;
                pending_brace_region_start = -1;
                pending_bins_start = -1;
                pending_bins_equals = false;
                break;

            default:
                break;
            }
        }
    }

    void process_simple_directive(const SimpleDirectiveSyntax& node) {
        int line = token_line(sm, node.directive);
        BufferID buffer = node.directive.location().buffer();
        if (node.kind == SyntaxKind::CellDefineDirective) {
            cell_stack.push_back({line, buffer});
        } else if (node.kind == SyntaxKind::EndCellDefineDirective) {
            if (!cell_stack.empty()) {
                emit(out, sm, cell_stack.back().buffer, cell_stack.back().start_line, line,
                     "region");
                cell_stack.pop_back();
            }
        }
    }

    int line_at(BufferID buffer, size_t offset) const {
        SourceLocation loc(buffer, offset);
        if (!loc.valid()) return -1;
        return (int)sm.getLineNumber(loc) - 1;
    }

    bool is_own_line_trivia(BufferID buffer, size_t offset) const {
        auto text = sm.getSourceText(buffer);
        if (offset > text.size()) return false;

        // Find line start, caching by line number to avoid repeated backward
        // scans for multiple trivia items on the same line.
        if (buffer != line_start_cache_buf_) {
            line_start_cache_.clear();
            line_start_cache_buf_ = buffer;
        }
        int    line_num  = line_at(buffer, offset);
        size_t line_start;
        auto   it        = line_start_cache_.find(line_num);
        if (it != line_start_cache_.end()) {
            line_start = it->second;
        } else {
            line_start = offset;
            while (line_start > 0 && text[line_start - 1] != '\n')
                --line_start;
            line_start_cache_[line_num] = line_start;
        }

        for (size_t i = line_start; i < offset; ++i) {
            unsigned char ch = static_cast<unsigned char>(text[i]);
            if (!std::isspace(ch)) return false;
        }
        return true;
    }
};

// ── import group collector ───────────────────────────────────────────────

static void collect_import_runs(const SourceManager& sm,
                                 const SyntaxList<MemberSyntax>& members,
                                 std::vector<FoldingRange>& out);

static void collect_import_folds(const SourceManager& sm, const SyntaxNode& root,
                                  std::vector<FoldingRange>& out) {
    // SyntaxTree::fromText uses parseGuess(), which can return the inner
    // declaration directly (e.g. ModuleDeclarationSyntax) instead of a
    // CompilationUnitSyntax when the file contains a single top-level construct.
    if (const auto* unit = root.as_if<CompilationUnitSyntax>())
        collect_import_runs(sm, unit->members, out);
    else if (const auto* mod = root.as_if<ModuleDeclarationSyntax>())
        collect_import_runs(sm, mod->members, out);
    else if (const auto* cls = root.as_if<ClassDeclarationSyntax>())
        collect_import_runs(sm, cls->items, out);
}

static void collect_import_runs(const SourceManager& sm,
                                 const SyntaxList<MemberSyntax>& members,
                                 std::vector<FoldingRange>& out) {
    int run_start = -1;
    int run_last  = -1;
    BufferID run_buffer{};

    auto flush = [&] {
        if (run_start >= 0 && run_last > run_start)
            emit(out, sm, run_buffer, run_start, run_last, "imports");
        run_start = -1;
        run_last  = -1;
        run_buffer = {};
    };

    for (const auto* member : members) {
        if (!member) { flush(); continue; }
        if (member->kind == SyntaxKind::PackageImportDeclaration) {
            int fl = first_line(sm, *member);
            int ll = last_line(sm, *member);
            if (run_start < 0) {
                run_start = fl;
                run_last  = ll;
                Token first = member->getFirstToken();
                if (first && first.location().valid())
                    run_buffer = first.location().buffer();
            } else if (fl == run_last + 1) {
                // Treat an import "run" as a source-adjacent visual group, not
                // merely consecutive PackageImportDeclarationSyntax members in the
                // AST.  Comments, blank lines, directives, and other trivia can be
                // invisible to the member list; folding across them would hide
                // explanatory text and merge intentionally separated import groups.
                run_last = ll;
            } else {
                flush();
                run_start = fl;
                run_last = ll;
                Token first = member->getFirstToken();
                if (first && first.location().valid())
                    run_buffer = first.location().buffer();
            }
        } else {
            flush();
            // Recurse into declaration bodies that can contain import statements.
            if (const auto* mod = member->as_if<ModuleDeclarationSyntax>())
                collect_import_runs(sm, mod->members, out);
            else if (const auto* cls = member->as_if<ClassDeclarationSyntax>())
                collect_import_runs(sm, cls->items, out);
        }
    }
    flush();
}

// ── final result normalization ───────────────────────────────────────────

static void normalize_folds(std::vector<FoldingRange>& folds) {
    std::sort(folds.begin(), folds.end(), [](const FoldingRange& a, const FoldingRange& b) {
        return std::tie(a.startLine, a.endLine, a.kind, a.startCharacter, a.endCharacter) <
               std::tie(b.startLine, b.endLine, b.kind, b.startCharacter, b.endCharacter);
    });

    folds.erase(std::unique(folds.begin(), folds.end(), [](const FoldingRange& a,
                                                           const FoldingRange& b) {
                    return a.startLine == b.startLine && a.endLine == b.endLine &&
                           a.kind == b.kind && a.startCharacter == b.startCharacter &&
                           a.endCharacter == b.endCharacter;
                }),
                folds.end());
}

static void append_directive_folds(std::vector<FoldingRange>& accepted_folds,
                                   const std::vector<FoldingRange>& directive_candidates) {
    //
    // Keep preprocessor conditional folds available at the directive line itself.
    // Earlier we tried suppressing a directive fold if it wrapped structural RTL
    // folds.  That made "za" inside always/if blocks nicer, but it also meant that
    // pressing "za" on the `ifdef line had no local directive fold and Neovim fell
    // back to folding the whole module.  The better compromise is to emit both:
    //
    //   `ifdef ... `else branch fold
    //   local always/begin/if folds inside that branch
    //
    // In inactive branches, local begin/end folds are recovered from slang's
    // disabled token list so the inner RTL blocks are still present.
    accepted_folds.insert(accepted_folds.end(), directive_candidates.begin(),
                          directive_candidates.end());
}

} // namespace

// ── public API ────────────────────────────────────────────────────────────

std::vector<FoldingRange> provide_folding_range(const Analyzer& analyzer,
                                                const FoldingRangeRequestParams& params) {
    auto state = analyzer.get_state(params.textDocument.uri.raw_uri_);
    if (!state || !state->tree) return {};

    auto&                    sm = *state->source_manager;
    std::vector<FoldingRange> out;

    FoldVisitor v{sm, out};
    state->tree->root().visit(v);
    v.flush_comment_run(); // flush any trailing comment run

    collect_import_folds(sm, state->tree->root(), out);
    append_directive_folds(out, v.directive_folds);

    normalize_folds(out);

    return out;
}
