#include "references.hpp"

static lsLocation to_lsp_location(const Location& location) {
    lsLocation loc;
    loc.uri.raw_uri_ = location.uri;
    loc.range.start = lsPosition(location.line, location.col);
    loc.range.end = lsPosition(location.end_line, location.end_col);
    return loc;
}

std::vector<lsLocation> provide_references(const Analyzer& analyzer,
                                           const TextDocumentReferences::Params& params) {
    const bool include_declaration =
        !params.context.includeDeclaration || *params.context.includeDeclaration;
    auto refs = analyzer.find_references(params.textDocument.uri.raw_uri_, params.position.line,
                                         params.position.character, include_declaration);

    std::vector<lsLocation> result;
    result.reserve(refs.size());
    for (const auto& ref : refs)
        result.push_back(to_lsp_location(ref));
    return result;
}
