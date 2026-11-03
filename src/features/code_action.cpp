#include "code_action.hpp"
#include "../lsp_position.hpp"
#include "../syntax_index.hpp"
#include "autoarg.hpp"
#include "autoff.hpp"
#include "autofunc.hpp"
#include "autoinst.hpp"
#include "autowire.hpp"
#include "formatter.hpp"
#include "lint.hpp"
#include <iostream>
#include <set>
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
    auto line = sm.getFileLineNumber(tok.location());
    return line > 0 ? (int)line - 1 : 0;
}

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
                                             const lsCodeActionParams& params,
                                             const std::vector<ParseDiagInfo>* published_lint) {
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

    // ── 7. Quick-fixes the current diagnostics carry ─────────────────────────
    // A request diagnostic is matched to the current one by position and message
    // (the server publishes no code).  Semantic diagnostics are already dropped
    // by the analyzer once the buffer is edited past the compile.
    // Parse and semantic diagnostics are cached, lint ones come from the
    // publish; lint is walked here only without those, when a request
    // diagnostic matched neither (and never for an empty request).
    std::vector<ParseDiagInfo> current = state->parse_diagnostics;
    {
        auto semantic = analyzer.semantic_diagnostics(uri);
        current.insert(current.end(), std::make_move_iterator(semantic.begin()),
                       std::make_move_iterator(semantic.end()));
    }
    std::vector<ParseDiagInfo> lint_diags;
    const auto& lint_pool = published_lint ? *published_lint : lint_diags;
    bool lint_done = published_lint != nullptr;
    auto find_match = [&](const std::vector<ParseDiagInfo>& pool, const lsDiagnostic& diag) {
        std::vector<const ParseDiagInfo*> out;
        for (const auto& cur : pool)
            if ((cur.uri.empty() || cur.uri == uri) && cur.line == diag.range.start.line &&
                cur.col == diag.range.start.character && cur.message == diag.message)
                out.push_back(&cur);
        return out;
    };
    std::set<std::string> seen_fixes;
    std::vector<CodeAction> safe_fixes, unsafe_fixes;
    for (const auto& diag : params.context.diagnostics) {
        auto matched = find_match(current, diag);
        if (matched.empty() && !lint_done && state->tree) {
            lint_done = true;
            try {
                // Same inputs as the publish path, so the sets are equal.
                std::shared_ptr<const ProjectIndexSnapshot> lint_index;
                if (config.lint.instance.enable && config.lint.instance.stale_instance_diagnostic)
                    lint_index = analyzer.project_index_snapshot();
                lint_diags = run_lint(*state, config.lint, lint_index.get());
            } catch (const SafeModeError&) {
                throw;
            } catch (...) {
            }
        }
        if (matched.empty())
            matched = find_match(lint_pool, diag);
        for (const auto* cur : matched) {
            for (const auto& fix : cur->fixes) {
                lsWorkspaceEdit we;
                we.changes = std::map<std::string, std::vector<lsTextEdit>>{};
                auto& edits = (*we.changes)[uri];
                std::string key = fix.title + (fix.safe ? "|s" : "|u");
                for (const auto& e : fix.edits) {
                    lsTextEdit te;
                    te.range.start = lsPosition(e.start_line, e.start_col);
                    te.range.end = lsPosition(e.end_line, e.end_col);
                    te.newText = e.new_text;
                    edits.push_back(std::move(te));
                    key += "|" + std::to_string(e.start_line) + ":" + std::to_string(e.start_col) +
                           "-" + std::to_string(e.end_line) + ":" + std::to_string(e.end_col) +
                           "=" + e.new_text;
                }
                if (!seen_fixes.insert(std::move(key)).second)
                    continue;
                CodeAction action;
                action.title = fix.safe ? fix.title : fix.title + " (may change behavior)";
                action.kind = optional<std::string>(std::string("quickfix"));
                action.edit = optional<lsWorkspaceEdit>(we);
                action.diagnostics = optional<std::vector<lsDiagnostic>>({diag});
                (fix.safe ? safe_fixes : unsafe_fixes).push_back(std::move(action));
            }
        }
    }
    for (auto* group : {&safe_fixes, &unsafe_fixes})
        for (auto& a : *group)
            actions.push_back(std::move(a));

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
