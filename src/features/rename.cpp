#include "rename.hpp"
#include "../string_utils.hpp"
#include <algorithm>
#include <array>
#include <fstream>
#include <sstream>
#include <unordered_map>

std::optional<PrepareRenameResult> prepare_rename(const Analyzer& analyzer,
                                                  const lsTextDocumentPositionParams& params) {
    auto ident = analyzer.identifier_at(params.textDocument.uri.raw_uri_, params.position.line,
                                        params.position.character);
    if (!ident || ident->name.empty())
        return std::nullopt;

    PrepareRenameResult result;
    result.range.start = lsPosition(ident->line, ident->col);
    result.range.end = lsPosition(ident->line, ident->end_col);
    result.placeholder = ident->name;
    return result;
}

namespace {

/// Text of the file a rename edit lands in: the open buffer's snapshot when the
/// file is open, otherwise the file on disk.  Read once per file per request.
const std::string* file_text(const Analyzer& analyzer, const std::string& uri,
                             std::unordered_map<std::string, std::string>& cache) {
    auto it = cache.find(uri);
    if (it != cache.end())
        return &it->second;

    if (auto state = analyzer.get_state(uri))
        return &cache.emplace(uri, state->text).first->second;

    std::ifstream in(path_from_file_uri(uri), std::ios::binary);
    if (!in)
        return nullptr;
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return &cache.emplace(uri, buffer.str()).first->second;
}

/// The source that an edit's range currently covers, or nullopt when the range
/// does not exist in the file.
std::optional<std::string_view> text_in_range(std::string_view text, const lsRange& range) {
    if (range.start.line != range.end.line || range.end.character < range.start.character)
        return std::nullopt;

    size_t pos = 0;
    for (int i = 0; i < range.start.line; ++i) {
        pos = text.find('\n', pos);
        if (pos == std::string_view::npos)
            return std::nullopt;
        ++pos;
    }
    size_t end = text.find('\n', pos);
    if (end == std::string_view::npos)
        end = text.size();
    if (end > pos && text[end - 1] == '\r')
        --end;

    const auto line = text.substr(pos, end - pos);
    if ((size_t)range.end.character > line.size())
        return std::nullopt;
    return line.substr(range.start.character,
                       (size_t)(range.end.character - range.start.character));
}

} // namespace

lsWorkspaceEdit provide_rename(const Analyzer& analyzer, const TextDocumentRename::Params& params) {
    lsWorkspaceEdit workspace_edit;
    auto ident = analyzer.identifier_at(params.textDocument.uri.raw_uri_, params.position.line,
                                        params.position.character);
    if (!ident || ident->name.empty())
        return workspace_edit;

    auto refs = analyzer.find_references(params.textDocument.uri.raw_uri_, params.position.line,
                                         params.position.character, true);
    std::map<std::string, std::vector<lsTextEdit>> changes;
    std::unordered_map<std::string, std::string> texts;
    for (const auto& ref : refs) {
        lsTextEdit edit;
        edit.range.start = lsPosition(ref.line, ref.col);
        edit.range.end = lsPosition(ref.end_line, ref.end_col);
        edit.newText = params.newName;

        // An identifier a macro pastes together (`` `MK_REG(status) `` declaring
        // `status_reg`) has no span in the source that spells it: the occurrence
        // is anchored at the macro argument but sized for the expanded name, so
        // applying it would overwrite whatever follows.  A rename that cannot
        // rewrite every occurrence must not rewrite some of them either — that
        // leaves the declaration and its uses disagreeing — so refuse the whole
        // request.
        const auto* text = file_text(analyzer, ref.uri, texts);
        const auto covered = text ? text_in_range(*text, edit.range) : std::nullopt;
        if (!covered || *covered != ident->name)
            return lsWorkspaceEdit{};

        changes[ref.uri].push_back(std::move(edit));
    }
    if (!changes.empty())
        workspace_edit.changes = std::move(changes);
    return workspace_edit;
}
