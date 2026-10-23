#include "document_highlight.hpp"

std::vector<lsDocumentHighlight> provide_document_highlight(const Analyzer& analyzer,
                                                           const lsTextDocumentPositionParams& params) {
    const auto& uri = params.textDocument.uri.raw_uri_;
    std::vector<lsDocumentHighlight> result;
    for (const auto& ref :
         analyzer.find_references(uri, params.position.line, params.position.character, true)) {
        if (ref.uri != uri)
            continue;
        lsDocumentHighlight highlight;
        highlight.range.start = lsPosition(ref.line, ref.col);
        highlight.range.end = lsPosition(ref.end_line, ref.end_col);
        highlight.kind = lsDocumentHighlightKind::Text;
        result.push_back(std::move(highlight));
    }
    return result;
}
