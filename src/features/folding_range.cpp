#include "folding_range.hpp"
#include "document_state.hpp"

#include <algorithm>
#include <cctype>
#include <tuple>
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

static void emit(std::vector<FoldingRange>& out, int start, int end,
                 const std::string& kind = "region") {
    if (start < 0 || end < 0 || start >= end) return;
    FoldingRange r;
    r.startLine      = start;
    r.endLine        = end;
    r.startCharacter = 0;
    r.endCharacter   = 0;
    r.kind           = kind;
    out.push_back(r);
}

// ── fold visitor ──────────────────────────────────────────────────────────

struct FoldVisitor : public SyntaxVisitor<FoldVisitor> {
    const SourceManager&       sm;
    std::vector<FoldingRange>& out;
    std::vector<FoldingRange>  directive_folds;

    struct PreprocessorBranch {
        int start_line{-1};
    };

    // preprocessor stack: current branch start for nested `ifdef / `elsif / `else blocks
    std::vector<PreprocessorBranch> pp_stack;

    // celldefine stack: line of the opening `celldefine
    std::vector<int> cell_stack;

    // consecutive line-comment run
    int comment_run_start{-1};
    int comment_run_last{-1};

    FoldVisitor(const SourceManager& sm, std::vector<FoldingRange>& out)
        : sm(sm), out(out) {}

    void flush_comment_run() {
        if (comment_run_start >= 0 && comment_run_last > comment_run_start)
            emit(out, comment_run_start, comment_run_last, "comment");
        comment_run_start = -1;
        comment_run_last  = -1;
    }

    void emit_directive(int start, int end) {
        emit(directive_folds, start, end, "region");
    }

    // ── top-level declarations ────────────────────────────────────────────

    // module / macromodule / interface / program / package (all use same type)
    void handle(const ModuleDeclarationSyntax& node) {
        emit(out, first_line(sm, node), last_line(sm, node));
        visitDefault(node);
    }

    void handle(const ClassDeclarationSyntax& node) {
        emit(out, first_line(sm, node), last_line(sm, node));
        visitDefault(node);
    }

    void handle(const CheckerDeclarationSyntax& node) {
        emit(out, first_line(sm, node), last_line(sm, node));
        visitDefault(node);
    }

    // primitive / UDP
    void handle(const UdpDeclarationSyntax& node) {
        emit(out, first_line(sm, node), last_line(sm, node));
        visitDefault(node);
    }

    void handle(const ConfigDeclarationSyntax& node) {
        emit(out, first_line(sm, node), last_line(sm, node));
        visitDefault(node);
    }

    void handle(const ClockingDeclarationSyntax& node) {
        emit(out, first_line(sm, node), last_line(sm, node));
        visitDefault(node);
    }

    // function / task (TaskDeclaration maps to FunctionDeclarationSyntax)
    void handle(const FunctionDeclarationSyntax& node) {
        emit(out, first_line(sm, node), last_line(sm, node));
        visitDefault(node);
    }

    // ── statement blocks ──────────────────────────────────────────────────

    // begin/end (SequentialBlockStatement) + fork/join* (ParallelBlockStatement)
    void handle(const BlockStatementSyntax& node) {
        emit(out, first_line(sm, node), last_line(sm, node));
        visitDefault(node);
    }

    // if / else if / else — fold the whole chain as one region
    void handle(const ConditionalStatementSyntax& node) {
        emit(out, first_line(sm, node), last_line(sm, node));
        visitDefault(node);
    }

    // case / casex / casez / endcase
    void handle(const CaseStatementSyntax& node) {
        emit(out, first_line(sm, node), last_line(sm, node));
        visitDefault(node);
    }

    // randcase / endcase
    void handle(const RandCaseStatementSyntax& node) {
        emit(out, first_line(sm, node), last_line(sm, node));
        visitDefault(node);
    }

    // individual multi-line case items
    void handle(const StandardCaseItemSyntax& node) {
        emit(out, first_line(sm, node), last_line(sm, node));
        visitDefault(node);
    }

    // ── verification constructs ───────────────────────────────────────────

    void handle(const ConstraintDeclarationSyntax& node) {
        emit(out, first_line(sm, node), last_line(sm, node));
        visitDefault(node);
    }

    void handle(const CovergroupDeclarationSyntax& node) {
        emit(out, first_line(sm, node), last_line(sm, node));
        visitDefault(node);
    }

    void handle(const PropertyDeclarationSyntax& node) {
        emit(out, first_line(sm, node), last_line(sm, node));
        visitDefault(node);
    }

    void handle(const SequenceDeclarationSyntax& node) {
        emit(out, first_line(sm, node), last_line(sm, node));
        visitDefault(node);
    }

    void handle(const SpecifyBlockSyntax& node) {
        emit(out, first_line(sm, node), last_line(sm, node));
        visitDefault(node);
    }

    // ── generate constructs ───────────────────────────────────────────────

    void handle(const GenerateRegionSyntax& node) {
        emit(out, first_line(sm, node), last_line(sm, node));
        visitDefault(node);
    }

    void handle(const LoopGenerateSyntax& node) {
        emit(out, first_line(sm, node), last_line(sm, node));
        visitDefault(node);
    }

    void handle(const IfGenerateSyntax& node) {
        int node_start = first_line(sm, node);
        int node_end   = last_line(sm, node);
        if (node.elseClause) {
            int else_line = token_line(sm, node.elseClause->elseKeyword);
            // fold the if arm
            emit(out, node_start, else_line - 1);
            // fold the else arm
            emit(out, else_line, node_end);
        } else {
            emit(out, node_start, node_end);
        }
        visitDefault(node);
    }

    void handle(const CaseGenerateSyntax& node) {
        emit(out, first_line(sm, node), last_line(sm, node));
        visitDefault(node);
    }

    void handle(const GenerateBlockSyntax& node) {
        emit(out, first_line(sm, node), last_line(sm, node));
        visitDefault(node);
    }

    // ── port / parameter lists ────────────────────────────────────────────

    void handle(const AnsiPortListSyntax& node) {
        emit(out, first_line(sm, node), last_line(sm, node));
        visitDefault(node);
    }

    void handle(const ParameterPortListSyntax& node) {
        emit(out, first_line(sm, node), last_line(sm, node));
        visitDefault(node);
    }

    // instance parameter override #(...)
    void handle(const ParameterValueAssignmentSyntax& node) {
        emit(out, first_line(sm, node), last_line(sm, node));
        visitDefault(node);
    }

    // instance connection list (...)
    void handle(const HierarchicalInstanceSyntax& node) {
        emit(out, first_line(sm, node), last_line(sm, node));
        visitDefault(node);
    }

    // multi-line argument lists
    void handle(const ArgumentListSyntax& node) {
        emit(out, first_line(sm, node), last_line(sm, node));
        visitDefault(node);
    }

    // ── preprocessor directives ───────────────────────────────────────────

    void handle(const ConditionalBranchDirectiveSyntax& node) {
        flush_comment_run();
        process_conditional_branch_directive(node);
        visitDefault(node);
    }

    void handle(const UnconditionalBranchDirectiveSyntax& node) {
        flush_comment_run();
        process_unconditional_branch_directive(node);
        visitDefault(node);
    }

    // `celldefine / `endcelldefine
    void handle(const SimpleDirectiveSyntax& node) {
        flush_comment_run();
        process_simple_directive(node);
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
        size_t pos = tok_offset >= trivia_total ? tok_offset - trivia_total : 0;

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
                emit(out, start, start + newlines, "comment");
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
            } else if (line == comment_run_last + 1) {
                comment_run_last = line;
            } else {
                flush_comment_run();
                comment_run_start = line;
                comment_run_last  = line;
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
        if (node.kind == SyntaxKind::IfDefDirective ||
            node.kind == SyntaxKind::IfNDefDirective) {
            pp_stack.push_back({line});
        } else if (node.kind == SyntaxKind::ElsIfDirective) {
            if (!pp_stack.empty()) {
                emit_directive(pp_stack.back().start_line, line - 1);
                pp_stack.back() = {line};
            }
        }
    }

    void process_unconditional_branch_directive(const UnconditionalBranchDirectiveSyntax& node) {
        collect_disabled_token_folds(node.disabledTokens);

        int line = token_line(sm, node.directive);
        if (node.kind == SyntaxKind::ElseDirective) {
            if (!pp_stack.empty()) {
                emit_directive(pp_stack.back().start_line, line - 1);
                pp_stack.back() = {line};
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
                emit_directive(branch.start_line, line);
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
        // This intentionally handles only keyword-driven begin/end style folds in
        // disabled branches.  Active branches continue to use the real AST visitor.
        std::vector<int> block_stack;
        int              pending_control_start{-1};

        auto mark_control_start = [&](const Token& token) {
            int line = token_line(sm, token);
            if (line >= 0)
                pending_control_start = line;
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
                    emit(out, block_stack.back(), end_line, "region");
                    block_stack.pop_back();
                }
                pending_control_start = -1;
                break;
            }

            default:
                break;
            }
        }
    }

    void process_simple_directive(const SimpleDirectiveSyntax& node) {
        int line = token_line(sm, node.directive);
        if (node.kind == SyntaxKind::CellDefineDirective) {
            cell_stack.push_back(line);
        } else if (node.kind == SyntaxKind::EndCellDefineDirective) {
            if (!cell_stack.empty()) {
                emit(out, cell_stack.back(), line, "region");
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

        size_t line_start = offset;
        while (line_start > 0 && text[line_start - 1] != '\n')
            --line_start;

        for (size_t i = line_start; i < offset; ++i) {
            unsigned char ch = static_cast<unsigned char>(text[i]);
            if (!std::isspace(ch)) return false;
        }
        return true;
    }
};

// ── import / include group collector ─────────────────────────────────────

static void collect_import_folds(const SourceManager& sm, const SyntaxNode& root,
                                  std::vector<FoldingRange>& out) {
    const auto* unit = root.as_if<CompilationUnitSyntax>();
    if (!unit) return;

    int run_start = -1;
    int run_last  = -1;

    auto flush = [&] {
        if (run_start >= 0 && run_last > run_start)
            emit(out, run_start, run_last, "imports");
        run_start = -1;
        run_last  = -1;
    };

    for (const auto* member : unit->members) {
        if (!member) continue;
        if (member->kind == SyntaxKind::PackageImportDeclaration) {
            int fl = first_line(sm, *member);
            int ll = last_line(sm, *member);
            if (run_start < 0) {
                run_start = fl;
                run_last  = ll;
            } else {
                run_last = ll;
            }
        } else {
            flush();
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
