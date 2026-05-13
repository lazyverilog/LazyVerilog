#include "rename.hpp"
#include <algorithm>
#include <array>

static bool is_sv_keyword(std::string_view word) {
    static constexpr auto keywords = std::to_array<std::string_view>({
        "accept_on", "alias",      "always",    "always_comb", "always_ff",  "always_latch",
        "and",       "assert",     "assign",    "automatic",   "begin",      "bind",
        "bins",      "bit",        "break",     "byte",        "case",       "casex",
        "casez",     "cell",       "chandle",   "class",       "clocking",   "config",
        "const",     "constraint", "context",   "continue",    "cover",      "covergroup",
        "coverpoint", "cross",     "deassign",  "default",     "defparam",   "design",
        "disable",   "do",         "edge",      "else",        "end",        "endcase",
        "endclass",  "endclocking", "endconfig", "endfunction", "endgenerate", "endmodule",
        "endpackage", "endprogram", "endproperty", "endspecify", "endtable", "endtask",
        "enum",      "event",      "expect",    "export",      "extends",    "extern",
        "final",     "for",        "force",     "foreach",     "forever",    "fork",
        "function",  "generate",   "genvar",    "if",          "iff",        "ifnone",
        "ignore_bins", "illegal_bins", "import", "incdir",      "include",    "initial",
        "inout",     "input",      "inside",    "int",         "integer",    "interface"});
    return std::find(keywords.begin(), keywords.end(), word) != keywords.end();
}

std::optional<PrepareRenameResult> prepare_rename(const Analyzer& analyzer,
                                                  const lsTextDocumentPositionParams& params) {
    auto ident = analyzer.identifier_at(params.textDocument.uri.raw_uri_, params.position.line,
                                        params.position.character);
    if (!ident || ident->name.empty() || is_sv_keyword(ident->name))
        return std::nullopt;

    PrepareRenameResult result;
    result.range.start = lsPosition(ident->line, ident->col);
    result.range.end = lsPosition(ident->line, ident->end_col);
    result.placeholder = ident->name;
    return result;
}

lsWorkspaceEdit provide_rename(const Analyzer& analyzer, const TextDocumentRename::Params& params) {
    lsWorkspaceEdit workspace_edit;
    auto ident = analyzer.identifier_at(params.textDocument.uri.raw_uri_, params.position.line,
                                        params.position.character);
    if (!ident || ident->name.empty())
        return workspace_edit;

    auto refs = analyzer.find_references(params.textDocument.uri.raw_uri_, params.position.line,
                                         params.position.character, true);
    std::map<std::string, std::vector<lsTextEdit>> changes;
    for (const auto& ref : refs) {
        lsTextEdit edit;
        edit.range.start = lsPosition(ref.line, ref.col);
        edit.range.end = lsPosition(ref.end_line, ref.end_col);
        edit.newText = params.newName;
        changes[ref.uri].push_back(std::move(edit));
    }
    if (!changes.empty())
        workspace_edit.changes = std::move(changes);
    return workspace_edit;
}
