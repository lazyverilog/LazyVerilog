#include "code_action.hpp"
#include "../syntax_index.hpp"
#include "autoarg.hpp"
#include "autoff.hpp"
#include "autofunc.hpp"
#include "autoinst.hpp"
#include "autowire.hpp"
#include "formatter.hpp"
#include <iostream>
#include <span>
#include <slang/syntax/AllSyntax.h>
#include <slang/syntax/SyntaxTree.h>
#include <slang/syntax/SyntaxVisitor.h>
#include <slang/text/SourceManager.h>

using namespace slang;
using namespace slang::syntax;

// ── Helpers ───────────────────────────────────────────────────────────────────

static lsWorkspaceEdit make_range_edit(const std::string& uri, int start_line, int start_col,
                                       int end_line, int end_col, const std::string& new_text) {
    lsWorkspaceEdit we;
    lsTextEdit edit;
    edit.range.start = lsPosition(start_line, start_col);
    edit.range.end = lsPosition(end_line, end_col);
    edit.newText = new_text;
    we.changes = std::map<std::string, std::vector<lsTextEdit>>{};
    (*we.changes)[uri] = {edit};
    return we;
}

static std::string format_emit_text(const std::string& text, const FormatOptions& options) {
    std::string formatted = format_source(text, options);
    while (!formatted.empty() && formatted.back() == '\n')
        formatted.pop_back();
    return formatted;
}

static size_t lsp_offset(const std::string& text, int line, int col) {
    int cur = 0;
    size_t pos = 0;
    while (pos < text.size() && cur < line) {
        if (text[pos] == '\n')
            ++cur;
        ++pos;
    }
    size_t line_start = pos;
    while (pos < text.size() && text[pos] != '\n' && (int)(pos - line_start) < col)
        ++pos;
    return pos;
}

static std::string format_replacement_at_column(
    const std::string& text,
    int column,
    const FormatOptions& options)
{
    std::string formatted = format_emit_text(text, options);
    if (column <= 0)
        return formatted;

    std::string prefix(column, ' ');
    std::string out;
    out.reserve(formatted.size() + prefix.size());
    for (size_t i = 0; i < formatted.size(); ++i) {
        out += formatted[i];
        if (formatted[i] == '\n' && i + 1 < formatted.size())
            out += prefix;
    }
    return out;
}

/// Leading whitespace of a 0-based line, or "" when the line has none.
static std::string leading_whitespace_of_line(const std::string& text, int line) {
    size_t pos = 0;
    for (int cur = 0; cur < line && pos < text.size(); ++pos)
        if (text[pos] == '\n')
            ++cur;
    size_t end = pos;
    while (end < text.size() && (text[end] == ' ' || text[end] == '\t'))
        ++end;
    return text.substr(pos, end - pos);
}

/// Prefix every line of @p text with @p indent.
///
/// The formatter re-indents an isolated snippet from column 0 because the
/// snippet carries no enclosing module, so an emitted instance came out flush
/// left no matter how the original was indented.  This edit replaces whole
/// lines, so the first line needs the prefix too.
static std::string indent_replacement_lines(const std::string& text, const std::string& indent) {
    if (indent.empty() || text.empty())
        return text;
    std::string out;
    out.reserve(text.size() + indent.size() * 4);
    out += indent;
    for (size_t i = 0; i < text.size(); ++i) {
        out += text[i];
        if (text[i] == '\n' && i + 1 < text.size())
            out += indent;
    }
    return out;
}

static int token_line(const SourceManager& sm, const slang::parsing::Token& tok) {
    if (!tok || !tok.location().valid())
        return 0;
    auto line = sm.getLineNumber(tok.location());
    return line > 0 ? (int)line - 1 : 0;
}

static int token_col(const SourceManager& sm, const slang::parsing::Token& tok) {
    if (!tok || !tok.location().valid())
        return 0;
    auto col = sm.getColumnNumber(tok.location());
    return col > 0 ? (int)col - 1 : 0;
}

struct QuickFixLocator : public SyntaxVisitor<QuickFixLocator> {
    const SourceManager& sm;
    int line;
    const CaseStatementSyntax* case_stmt{nullptr};
    const FunctionDeclarationSyntax* function_decl{nullptr};
    const FunctionDeclarationSyntax* task_decl{nullptr};

    QuickFixLocator(const SourceManager& sm, int line) : sm(sm), line(line) {}

    bool contains_line(const SyntaxNode& node) const {
        auto first = node.getFirstToken();
        auto last = node.getLastToken();
        if (!first || !last || !first.location().valid() || !last.location().valid())
            return false;
        return token_line(sm, first) <= line && line <= token_line(sm, last);
    }

    void handle(const CaseStatementSyntax& node) {
        if (!case_stmt && contains_line(node))
            case_stmt = &node;
        visitDefault(node);
    }
    void handle(const FunctionDeclarationSyntax& node) {
        if (token_line(sm, node.getFirstToken()) == line) {
            if (node.kind == SyntaxKind::TaskDeclaration)
                task_decl = &node;
            else
                function_decl = &node;
        }
        visitDefault(node);
    }
};

// True when `line` falls inside a class body.  The RTL generators below emit
// net declarations and always_ff/always_comb blocks, none of which are legal
// class items, so offering them there is pure noise.  Interfaces and programs
// do accept those constructs and are deliberately not excluded.
static bool line_inside_class_body(const SyntaxTree& tree, int line) {
    struct Visitor : public SyntaxVisitor<Visitor> {
        const SourceManager& sm;
        int line;
        bool found{false};

        Visitor(const SourceManager& sm, int line) : sm(sm), line(line) {}

        void handle(const ClassDeclarationSyntax& node) {
            auto first = node.getFirstToken();
            auto last = node.getLastToken();
            if (first && last && first.location().valid() && last.location().valid() &&
                token_line(sm, first) <= line && line <= token_line(sm, last))
                found = true;
            visitDefault(node);
        }
    } visitor(tree.sourceManager(), line);
    tree.root().visit(visitor);
    return visitor.found;
}

// ── Main ──────────────────────────────────────────────────────────────────────

std::vector<CodeAction> provide_code_actions(const Analyzer& analyzer, const Config& config,
                                             const lsCodeActionParams& params) {
    std::vector<CodeAction> actions;

    const std::string& uri = params.textDocument.uri.raw_uri_;
    int line = params.range.start.line;
    int col = params.range.start.character;

    auto state = analyzer.get_state(uri);
    if (!state)
        return actions;

    // Cross-file fallback for AutoInst.  The current file itself is resolved
    // AST-first inside autoinst_impl(); other open buffers and project files
    // stay as separate index layers so code actions do not materialize a flat
    // all-shard project index on request paths.
    std::shared_ptr<const std::vector<OpenIndexShard>> opened_shards;
    std::shared_ptr<const ProjectIndexSnapshot> project_index;
    if (state->tree) {
        opened_shards = analyzer.opened_file_index_shards(uri);
        project_index = analyzer.project_index_snapshot();
    }

    // The generators below produce module-level RTL, which a class body cannot
    // hold.  Offering them inside a class is noise on every UVM file.
    const bool in_class_body = state->tree && line_inside_class_body(*state->tree, line);

    // ── 1. AutoInst ──────────────────────────────────────────────────────────
    try {
        if (state->tree) {
            auto result = autoinst_impl(*state, line, col,
                                        opened_shards ? std::span<const OpenIndexShard>(*opened_shards)
                                                      : std::span<const OpenIndexShard>{},
                                        project_index.get());
            if (result) {
                std::string formatted = indent_replacement_lines(
                    format_emit_text(format_autoinst(*result, state->text, config.autoinst),
                                     config.format),
                    leading_whitespace_of_line(state->text, result->line_start));
                // Replace the instantiation range
                auto we = make_range_edit(uri, result->line_start, 0, result->line_end + 1, 0,
                                          formatted + "\n");
                CodeAction action;
                action.title = "AutoInst: expand " + result->module_name;
                action.kind = optional<std::string>(std::string("refactor.rewrite"));
                action.edit = optional<lsWorkspaceEdit>(we);
                actions.push_back(std::move(action));
            }
        }
    } catch (const SafeModeError&) {
        throw;
    } catch (const std::exception& e) {
        std::cerr << "[lazyverilog] codeAction autoinst: " << e.what() << "\n";
    }

    // ── 2. AutoArg ───────────────────────────────────────────────────────────
    try {
        if (state->tree) {
            auto result = autoarg_impl(*state, line, col);
            if (result) {
                size_t line_start = lsp_offset(state->text, result->open_line, 0);
                size_t open = lsp_offset(state->text, result->open_line, result->open_col);
                std::string line_prefix = state->text.substr(line_start, open - line_start);
                // AutoArg's generator knows which ports belong in the non-ANSI module header,
                // but the project formatter owns final whitespace/layout decisions.  Format the
                // complete replacement header fragment instead of returning the raw generated
                // port list so AutoArg behaves like the other auto-code actions.
                FormatOptions autoarg_format = config.format;
                std::string formatted = format_emit_text(
                    line_prefix + format_autoarg(*result, config.autoarg, autoarg_format),
                    autoarg_format);
                auto we = make_range_edit(uri, result->open_line, 0,
                                          result->end_line, result->end_col, formatted);
                CodeAction action;
                action.title = "AutoArg: generate port list for " + result->module_name;
                action.kind = optional<std::string>(std::string("refactor.rewrite"));
                action.edit = optional<lsWorkspaceEdit>(we);
                actions.push_back(std::move(action));
            }
        }
    } catch (const SafeModeError&) {
        throw;
    } catch (const std::exception& e) {
        std::cerr << "[lazyverilog] codeAction autoarg: " << e.what() << "\n";
    }

    // ── 3. AutoFunc ──────────────────────────────────────────────────────────
    try {
        auto we = autofunc(analyzer, uri, line, col, config.autofunc);
        if (we) {
            if (we->changes) {
                for (auto& [_, edits] : *we->changes) {
                    for (auto& edit : edits) {
                        edit.newText = format_replacement_at_column(
                            edit.newText, edit.range.start.character, config.format);
                    }
                }
            }
            CodeAction action;
            action.title = "AutoFunc: expand function call";
            action.kind = optional<std::string>(std::string("refactor.rewrite"));
            action.edit = optional<lsWorkspaceEdit>(*we);
            actions.push_back(std::move(action));
        }
    } catch (const SafeModeError&) {
        throw;
    } catch (const std::exception& e) {
        std::cerr << "[lazyverilog] codeAction autofunc: " << e.what() << "\n";
    }

    // ── 4. AutoFF (single) ───────────────────────────────────────────────────
    try {
        auto preview = preview_autoff(*state, line, config.autoff.register_pattern);
        if (!preview.has_error && !preview.pairs.empty()) {
            CodeAction action;
            action.title = "AutoFF: insert FF assignments";
            action.kind = optional<std::string>(std::string("refactor.rewrite"));
            // Return the preview command. The Neovim plugin shows the confirmation
            // floating window from preview data, then calls autoffApply if accepted.
            lsCommandWithAny cmd;
            cmd.title = "AutoFF: insert FF assignments";
            cmd.command = "lazyverilog.autoffPreview";
            // args: [uri, line] — encode as JSON
            lsp::Any uri_arg, line_arg;
            uri_arg.SetJsonString("\"" + uri + "\"", lsp::Any::kUnKnown);
            line_arg.SetJsonString(std::to_string(line), lsp::Any::kUnKnown);
            cmd.arguments = optional<std::vector<lsp::Any>>({uri_arg, line_arg});
            action.command = optional<lsCommandWithAny>(cmd);
            actions.push_back(std::move(action));
        }
    } catch (const std::exception& e) {
        std::cerr << "[lazyverilog] codeAction autoff: " << e.what() << "\n";
    }

    // ── 5. AutoWire ──────────────────────────────────────────────────────────
    try {
        if (state->tree && !in_class_body) {
            CodeAction action;
            action.title = "AutoWire: declare missing signals";
            action.kind = optional<std::string>(std::string("refactor.rewrite"));
            lsCommandWithAny cmd;
            cmd.title = "AutoWire: declare missing signals";
            cmd.command = "lazyverilog.autowire";
            lsp::Any uri_arg, line_arg;
            uri_arg.SetJsonString("\"" + uri + "\"", lsp::Any::kUnKnown);
            line_arg.SetJsonString(std::to_string(line), lsp::Any::kUnKnown);
            cmd.arguments = optional<std::vector<lsp::Any>>({uri_arg, line_arg});
            action.command = optional<lsCommandWithAny>(cmd);
            actions.push_back(std::move(action));
        }
    } catch (const std::exception& e) {
        std::cerr << "[lazyverilog] codeAction autowire: " << e.what() << "\n";
    }

    // ── 6. AutoFF All ────────────────────────────────────────────────────────
    try {
        if (!in_class_body) {
            CodeAction action;
            action.title = "AutoFF All: insert all FF assignments";
            action.kind = optional<std::string>(std::string("refactor.rewrite"));
            lsCommandWithAny cmd;
            cmd.title = "AutoFF All";
            cmd.command = "lazyverilog.autoffAllPreview";
            lsp::Any uri_arg;
            uri_arg.SetJsonString("\"" + uri + "\"", lsp::Any::kUnKnown);
            cmd.arguments = optional<std::vector<lsp::Any>>({uri_arg});
            action.command = optional<lsCommandWithAny>(cmd);
            actions.push_back(std::move(action));
        }
    } catch (...) {
    }

    // ── 7. Lint quick-fixes from diagnostics ─────────────────────────────────
    // Pre-split lines once for all diagnostic handlers
    std::vector<std::string> doc_lines;
    {
        size_t start = 0;
        const std::string& t = state->text;
        while (start <= t.size()) {
            size_t end = t.find('\n', start);
            if (end == std::string::npos) {
                doc_lines.push_back(t.substr(start));
                break;
            }
            doc_lines.push_back(t.substr(start, end - start));
            start = end + 1;
        }
    }

    const CaseStatementSyntax* case_stmt = nullptr;
    if (state->tree) {
        QuickFixLocator locator(state->tree->sourceManager(), line);
        state->tree->root().visit(locator);
        case_stmt = locator.case_stmt;
    }

    for (const auto& diag : params.context.diagnostics) {
        try {
            const std::string& msg = diag.message;
            if (msg.find("case_missing_default") != std::string::npos ||
                msg.find("case statement missing default") != std::string::npos) {
                int endcase_line = -1;
                if (case_stmt)
                    endcase_line = token_line(state->tree->sourceManager(), case_stmt->endcase);
                if (endcase_line >= 0) {
                    std::string case_indent;
                    for (char c : doc_lines[line]) {
                        if (c == ' ' || c == '\t')
                            case_indent += c;
                        else
                            break;
                    }
                    auto we = make_range_edit(uri, endcase_line, 0, endcase_line, 0,
                                              case_indent + "  default: ;\n");
                    CodeAction action;
                    action.title = "Add default case";
                    action.kind = optional<std::string>(std::string("quickfix"));
                    action.edit = optional<lsWorkspaceEdit>(we);
                    action.diagnostics = optional<std::vector<lsDiagnostic>>({diag});
                    actions.push_back(std::move(action));
                }
            } else if (msg.find("functions_automatic") != std::string::npos ||
                       msg.find("function declaration should use 'automatic'") !=
                           std::string::npos ||
                       msg.find("explicit_function_lifetime") != std::string::npos ||
                       msg.find("function declaration missing explicit lifetime") !=
                           std::string::npos) {
                int diag_line = diag.range.start.line;
                if (diag_line < (int)doc_lines.size() && state->tree) {
                    QuickFixLocator locator(state->tree->sourceManager(), diag_line);
                    state->tree->root().visit(locator);
                    if (locator.function_decl) {
                        int insert_col = token_col(state->tree->sourceManager(),
                                                   locator.function_decl->getFirstToken()) +
                                         8;
                        auto we = make_range_edit(uri, diag_line, insert_col, diag_line, insert_col,
                                                  " automatic");
                        CodeAction action;
                        action.title = "Add 'automatic' lifetime";
                        action.kind = optional<std::string>(std::string("quickfix"));
                        action.edit = optional<lsWorkspaceEdit>(we);
                        action.diagnostics = optional<std::vector<lsDiagnostic>>({diag});
                        actions.push_back(std::move(action));
                    }
                }
            } else if (msg.find("explicit_task_lifetime") != std::string::npos ||
                       msg.find("task declaration missing explicit lifetime") !=
                           std::string::npos) {
                int diag_line = diag.range.start.line;
                if (diag_line < (int)doc_lines.size() && state->tree) {
                    QuickFixLocator locator(state->tree->sourceManager(), diag_line);
                    state->tree->root().visit(locator);
                    if (locator.task_decl) {
                        int insert_col = token_col(state->tree->sourceManager(),
                                                   locator.task_decl->getFirstToken()) +
                                         4;
                        auto we = make_range_edit(uri, diag_line, insert_col, diag_line, insert_col,
                                                  " automatic");
                        CodeAction action;
                        action.title = "Add 'automatic' lifetime to task";
                        action.kind = optional<std::string>(std::string("quickfix"));
                        action.edit = optional<lsWorkspaceEdit>(we);
                        action.diagnostics = optional<std::vector<lsDiagnostic>>({diag});
                        actions.push_back(std::move(action));
                    }
                }
            }
        } catch (...) {
        }
    }

    // ── 8. Template snippets ─────────────────────────────────────────────────
    if (!in_class_body) {
        CodeAction ff_snippet;
        ff_snippet.title = "Insert always_ff block";
        ff_snippet.kind = optional<std::string>(std::string("refactor.rewrite"));
        std::string ff_text = "always_ff @(posedge clk or negedge rst_n) begin\n"
                              "  if (!rst_n) begin\n"
                              "  end else begin\n"
                              "  end\n"
                              "end\n";
        auto ff_we = make_range_edit(uri, line, 0, line, 0, ff_text);
        ff_snippet.edit = optional<lsWorkspaceEdit>(ff_we);
        actions.push_back(std::move(ff_snippet));
    }
    if (!in_class_body) {
        CodeAction comb_snippet;
        comb_snippet.title = "Insert always_comb block";
        comb_snippet.kind = optional<std::string>(std::string("refactor.rewrite"));
        std::string comb_text = "always_comb begin\n"
                                "end\n";
        auto comb_we = make_range_edit(uri, line, 0, line, 0, comb_text);
        comb_snippet.edit = optional<lsWorkspaceEdit>(comb_we);
        actions.push_back(std::move(comb_snippet));
    }

    return actions;
}
