#include "code_action.hpp"
#include "autoinst.hpp"
#include "autoarg.hpp"
#include "autowire.hpp"
#include "autoff.hpp"
#include "autofunc.hpp"
#include "../syntax_index.hpp"
#include <iostream>

// ── Helpers ───────────────────────────────────────────────────────────────────

static lsWorkspaceEdit make_full_doc_edit(const std::string& uri, const std::string& new_text) {
    lsWorkspaceEdit we;
    lsTextEdit edit;
    edit.range.start = lsPosition(0, 0);
    // large end position — client will clamp
    edit.range.end = lsPosition(999999, 0);
    edit.newText = new_text;
    we.changes = std::map<std::string, std::vector<lsTextEdit>>{};
    (*we.changes)[uri] = {edit};
    return we;
}

static lsWorkspaceEdit make_range_edit(
    const std::string& uri,
    int start_line, int start_col,
    int end_line, int end_col,
    const std::string& new_text)
{
    lsWorkspaceEdit we;
    lsTextEdit edit;
    edit.range.start = lsPosition(start_line, start_col);
    edit.range.end = lsPosition(end_line, end_col);
    edit.newText = new_text;
    we.changes = std::map<std::string, std::vector<lsTextEdit>>{};
    (*we.changes)[uri] = {edit};
    return we;
}

// ── Main ──────────────────────────────────────────────────────────────────────

std::vector<CodeAction> provide_code_actions(
    const Analyzer& analyzer, const Config& config,
    const lsCodeActionParams& params)
{
    std::vector<CodeAction> actions;

    const std::string& uri = params.textDocument.uri.raw_uri_;
    int line = params.range.start.line;
    int col = params.range.start.character;

    auto state = analyzer.get_state(uri);
    if (!state) return actions;

    // ── 1. AutoInst ──────────────────────────────────────────────────────────
    try {
        if (state->tree) {
            auto idx = SyntaxIndex::build(*state->tree, state->text);
            auto result = autoinst_impl(*state, line, col, idx);
            if (result) {
                std::string formatted = format_autoinst(*result, state->text, config.autoinst);
                // Replace the instantiation range
                auto we = make_range_edit(uri,
                    result->line_start, 0,
                    result->line_end + 1, 0,
                    formatted + "\n");
                CodeAction action;
                action.title = "AutoInst: expand " + result->module_name;
                action.kind = optional<std::string>(std::string("refactor.rewrite"));
                action.edit = optional<lsWorkspaceEdit>(we);
                actions.push_back(std::move(action));
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[lazyverilog] codeAction autoinst: " << e.what() << "\n";
    }

    // ── 2. AutoArg ───────────────────────────────────────────────────────────
    try {
        if (state->tree) {
            auto result = autoarg_impl(*state, line, col);
            if (result) {
                std::string formatted = format_autoarg(*result, config.autoarg);
                auto we = make_range_edit(uri,
                    result->open_line, result->open_col,
                    result->end_line, result->end_col,
                    formatted);
                CodeAction action;
                action.title = "AutoArg: generate port list for " + result->module_name;
                action.kind = optional<std::string>(std::string("refactor.rewrite"));
                action.edit = optional<lsWorkspaceEdit>(we);
                actions.push_back(std::move(action));
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[lazyverilog] codeAction autoarg: " << e.what() << "\n";
    }

    // ── 3. AutoFunc ──────────────────────────────────────────────────────────
    try {
        auto we = autofunc(analyzer, uri, line, col, config.autofunc);
        if (we) {
            CodeAction action;
            action.title = "AutoFunc: expand function call";
            action.kind = optional<std::string>(std::string("refactor.rewrite"));
            action.edit = optional<lsWorkspaceEdit>(*we);
            actions.push_back(std::move(action));
        }
    } catch (const std::exception& e) {
        std::cerr << "[lazyverilog] codeAction autofunc: " << e.what() << "\n";
    }

    // ── 4. AutoFF (single) ───────────────────────────────────────────────────
    try {
        auto preview = preview_autoff(*state, line, config.lint.naming.register_pattern);
        if (!preview.has_error && !preview.pairs.empty()) {
            CodeAction action;
            action.title = "AutoFF: insert FF assignments";
            action.kind = optional<std::string>(std::string("refactor.rewrite"));
            // Return as command — client calls autoffPreview
            lsCommandWithAny cmd;
            cmd.title = "AutoFF: insert FF assignments";
            cmd.command = "lazyverilogpy.autoffPreview";
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
        if (state->tree) {
            auto idx = SyntaxIndex::build(*state->tree, state->text);
            std::string new_source = autowire_apply(*state, idx, config.autowire);
            if (new_source != state->text) {
                auto we = make_full_doc_edit(uri, new_source);
                CodeAction action;
                action.title = "AutoWire: declare missing signals";
                action.kind = optional<std::string>(std::string("refactor.rewrite"));
                action.edit = optional<lsWorkspaceEdit>(we);
                actions.push_back(std::move(action));
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[lazyverilog] codeAction autowire: " << e.what() << "\n";
    }

    // ── 6. AutoFF All ────────────────────────────────────────────────────────
    try {
        CodeAction action;
        action.title = "AutoFF All: insert all FF assignments";
        action.kind = optional<std::string>(std::string("refactor.rewrite"));
        lsCommandWithAny cmd;
        cmd.title = "AutoFF All";
        cmd.command = "lazyverilogpy.autoffAllPreview";
        lsp::Any uri_arg;
        uri_arg.SetJsonString("\"" + uri + "\"", lsp::Any::kUnKnown);
        cmd.arguments = optional<std::vector<lsp::Any>>({uri_arg});
        action.command = optional<lsCommandWithAny>(cmd);
        actions.push_back(std::move(action));
    } catch (...) {}

    // ── 7. Lint quick-fixes from diagnostics ─────────────────────────────────
    for (const auto& diag : params.context.diagnostics) {
        try {
            const std::string& msg = diag.message;
            if (msg.find("case_missing_default") != std::string::npos ||
                msg.find("case statement missing default") != std::string::npos)
            {
                // Find endcase line near cursor and insert default before it
                auto lines = [&]() {
                    std::vector<std::string> ls;
                    size_t start = 0;
                    const std::string& t = state->text;
                    while (start <= t.size()) {
                        size_t end = t.find('\n', start);
                        if (end == std::string::npos) { ls.push_back(t.substr(start)); break; }
                        ls.push_back(t.substr(start, end - start));
                        start = end + 1;
                    }
                    return ls;
                }();

                int endcase_line = -1;
                for (int i = line; i < (int)lines.size(); ++i) {
                    if (lines[i].find("endcase") != std::string::npos) {
                        endcase_line = i;
                        break;
                    }
                }
                if (endcase_line >= 0) {
                    // Detect indent from case line
                    std::string case_indent;
                    for (char c : lines[line]) {
                        if (c == ' ' || c == '\t') case_indent += c;
                        else break;
                    }
                    std::string insert = case_indent + "  default: ;\n";
                    auto we = make_range_edit(uri, endcase_line, 0, endcase_line, 0, insert);
                    CodeAction action;
                    action.title = "Add default case";
                    action.kind = optional<std::string>(std::string("quickfix"));
                    action.edit = optional<lsWorkspaceEdit>(we);
                    action.diagnostics = optional<std::vector<lsDiagnostic>>({diag});
                    actions.push_back(std::move(action));
                }
            } else if (msg.find("functions_automatic") != std::string::npos ||
                       msg.find("function declaration should use 'automatic'") != std::string::npos ||
                       msg.find("explicit_function_lifetime") != std::string::npos ||
                       msg.find("function declaration missing explicit lifetime") != std::string::npos)
            {
                // Insert 'automatic' after 'function' keyword on the diagnostic line
                auto diag_line = diag.range.start.line;
                auto lines_v = [&]() {
                    std::vector<std::string> ls;
                    size_t start = 0;
                    const std::string& t = state->text;
                    while (start <= t.size()) {
                        size_t end = t.find('\n', start);
                        if (end == std::string::npos) { ls.push_back(t.substr(start)); break; }
                        ls.push_back(t.substr(start, end - start));
                        start = end + 1;
                    }
                    return ls;
                }();
                if (diag_line < (int)lines_v.size()) {
                    const std::string& src = lines_v[diag_line];
                    size_t func_pos = src.find("function");
                    if (func_pos != std::string::npos) {
                        int insert_col = (int)func_pos + 8; // after "function"
                        auto we = make_range_edit(uri, diag_line, insert_col, diag_line, insert_col, " automatic");
                        CodeAction action;
                        action.title = "Add 'automatic' lifetime";
                        action.kind = optional<std::string>(std::string("quickfix"));
                        action.edit = optional<lsWorkspaceEdit>(we);
                        action.diagnostics = optional<std::vector<lsDiagnostic>>({diag});
                        actions.push_back(std::move(action));
                    }
                }
            } else if (msg.find("explicit_task_lifetime") != std::string::npos ||
                       msg.find("task declaration missing explicit lifetime") != std::string::npos)
            {
                auto diag_line = diag.range.start.line;
                auto lines_v = [&]() {
                    std::vector<std::string> ls;
                    size_t start = 0;
                    const std::string& t = state->text;
                    while (start <= t.size()) {
                        size_t end = t.find('\n', start);
                        if (end == std::string::npos) { ls.push_back(t.substr(start)); break; }
                        ls.push_back(t.substr(start, end - start));
                        start = end + 1;
                    }
                    return ls;
                }();
                if (diag_line < (int)lines_v.size()) {
                    const std::string& src = lines_v[diag_line];
                    size_t task_pos = src.find("task");
                    if (task_pos != std::string::npos) {
                        int insert_col = (int)task_pos + 4;
                        auto we = make_range_edit(uri, diag_line, insert_col, diag_line, insert_col, " automatic");
                        CodeAction action;
                        action.title = "Add 'automatic' lifetime to task";
                        action.kind = optional<std::string>(std::string("quickfix"));
                        action.edit = optional<lsWorkspaceEdit>(we);
                        action.diagnostics = optional<std::vector<lsDiagnostic>>({diag});
                        actions.push_back(std::move(action));
                    }
                }
            }
        } catch (...) {}
    }

    // ── 8. Template snippets ─────────────────────────────────────────────────
    {
        CodeAction ff_snippet;
        ff_snippet.title = "Insert always_ff block";
        ff_snippet.kind = optional<std::string>(std::string("refactor.rewrite"));
        std::string ff_text =
            "always_ff @(posedge clk or negedge rst_n) begin\n"
            "  if (!rst_n) begin\n"
            "  end else begin\n"
            "  end\n"
            "end\n";
        auto ff_we = make_range_edit(uri, line, 0, line, 0, ff_text);
        ff_snippet.edit = optional<lsWorkspaceEdit>(ff_we);
        actions.push_back(std::move(ff_snippet));
    }
    {
        CodeAction comb_snippet;
        comb_snippet.title = "Insert always_comb block";
        comb_snippet.kind = optional<std::string>(std::string("refactor.rewrite"));
        std::string comb_text =
            "always_comb begin\n"
            "end\n";
        auto comb_we = make_range_edit(uri, line, 0, line, 0, comb_text);
        comb_snippet.edit = optional<lsWorkspaceEdit>(comb_we);
        actions.push_back(std::move(comb_snippet));
    }

    return actions;
}
