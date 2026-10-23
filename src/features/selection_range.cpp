#include "selection_range.hpp"
#include "folding_range.hpp"
#include "string_utils.hpp"

#include <slang/syntax/SyntaxNode.h>
#include <slang/syntax/SyntaxTree.h>

#include <algorithm>
#include <cctype>

namespace {

struct ByteRange {
    size_t start;
    size_t end;
};

/// Byte offset <-> LSP position over one document's text.
class Lines {
  public:
    explicit Lines(const std::string& text) : text_(text) {
        starts_.push_back(0);
        for (size_t i = 0; i < text.size(); ++i)
            if (text[i] == '\n')
                starts_.push_back(i + 1);
    }

    /// Clamped: a line past the end is the end of the document.
    size_t offset(lsPosition pos) const {
        if (static_cast<size_t>(pos.line) >= starts_.size())
            return text_.size();
        return lsp_col_to_byte_offset(text_, starts_[pos.line], static_cast<int>(pos.character));
    }

    lsPosition position(size_t offset) const {
        const auto it = std::upper_bound(starts_.begin(), starts_.end(), offset);
        const auto line = static_cast<int>(it - starts_.begin()) - 1;
        return lsPosition(line, lsp_column_from_byte_offset(text_, starts_[line], offset));
    }

    size_t line_start(int line) const {
        return static_cast<size_t>(line) < starts_.size() ? starts_[line] : text_.size();
    }

  private:
    const std::string& text_;
    std::vector<size_t> starts_;
};

/// Chain innermost first, keeping only ranges that strictly grow.
SelectionRange build_chain(const std::vector<ByteRange>& ranges, const Lines& lines,
                           std::list<SelectionRange>& parents) {
    std::vector<ByteRange> kept;
    for (const auto& r : ranges) {
        if (!kept.empty()) {
            const auto& prev = kept.back();
            const bool grows = r.start <= prev.start && prev.end <= r.end &&
                               (r.start != prev.start || r.end != prev.end);
            if (!grows)
                continue;
        }
        kept.push_back(r);
    }

    SelectionRange* parent = nullptr;
    for (size_t i = kept.size(); i-- > 1;) {
        SelectionRange node;
        node.range.start = lines.position(kept[i].start);
        node.range.end = lines.position(kept[i].end);
        if (parent)
            node.parent = parent;
        parents.push_back(node);
        parent = &parents.back();
    }
    SelectionRange first;
    first.range.start = lines.position(kept[0].start);
    first.range.end = lines.position(kept[0].end);
    if (parent)
        first.parent = parent;
    return first;
}

bool is_identifier_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$';
}

ByteRange word_at(const std::string& text, size_t offset) {
    if (offset >= text.size() || !is_identifier_char(text[offset]))
        return {offset, offset};
    size_t start = offset;
    while (start > 0 && is_identifier_char(text[start - 1]))
        --start;
    size_t end = offset;
    while (end < text.size() && is_identifier_char(text[end]))
        ++end;
    return {start, end};
}

/// One root-to-leaf descent: only the children containing `offset` are entered.
void descend(const slang::syntax::SyntaxNode& node, size_t offset, slang::BufferID buffer,
             std::vector<ByteRange>& outer_to_inner, std::optional<ByteRange>& token) {
    auto in_file = [&](slang::SourceRange r) {
        return r.start().buffer() == buffer && r.end().buffer() == buffer;
    };
    for (size_t i = 0; i < node.getChildCount(); ++i) {
        if (const auto* child = node.childNode(i)) {
            const auto r = child->sourceRange();
            if (in_file(r) && r.start().offset() <= offset && offset < r.end().offset()) {
                outer_to_inner.push_back({r.start().offset(), r.end().offset()});
                descend(*child, offset, buffer, outer_to_inner, token);
                return;
            }
        } else if (const auto t = node.childToken(i); t && !t.isMissing()) {
            const auto r = t.range();
            if (in_file(r) && r.start().offset() <= offset && offset < r.end().offset()) {
                token = ByteRange{r.start().offset(), r.end().offset()};
                return;
            }
        }
    }
}

} // namespace

SelectionRanges selection_range_without_tree(const std::string& text,
                                             const std::vector<FoldingRange>& folds, lsPosition pos) {
    const Lines lines(text);
    const size_t offset = lines.offset(pos);
    const int line = lines.position(offset).line;

    std::vector<ByteRange> fold_ranges;
    for (const auto& fold : folds) {
        if (fold.startLine <= line && line <= fold.endLine)
            fold_ranges.push_back({lines.line_start(fold.startLine), lines.line_start(fold.endLine + 1)});
    }
    std::sort(fold_ranges.begin(), fold_ranges.end(), [](const ByteRange& a, const ByteRange& b) {
        return a.end - a.start < b.end - b.start;
    });

    std::vector<ByteRange> ranges{word_at(text, offset)};
    ranges.insert(ranges.end(), fold_ranges.begin(), fold_ranges.end());
    ranges.push_back({0, text.size()});

    SelectionRanges answer;
    answer.result.push_back(build_chain(ranges, lines, answer.parents));
    return answer;
}

SelectionRanges provide_selection_range(const Analyzer& analyzer, const SelectionRangeParams& params) {
    SelectionRanges answer;
    const auto state = analyzer.get_state(params.textDocument.uri.raw_uri_);
    if (!state)
        return answer;

    if (!state->tree) {
        FoldingRangeRequestParams fold_params;
        fold_params.textDocument = params.textDocument;
        const auto folds = provide_folding_range_shared(analyzer, fold_params);
        static const std::vector<FoldingRange> no_folds;
        for (const auto& pos : params.positions) {
            auto one = selection_range_without_tree(state->text, folds ? *folds : no_folds, pos);
            answer.result.push_back(one.result.front());
            answer.parents.splice(answer.parents.end(), one.parents);
        }
        return answer;
    }

    const Lines lines(state->text);
    const auto buffers = state->tree->getSourceBufferIds();
    for (const auto& pos : params.positions) {
        const size_t offset = lines.offset(pos);
        std::vector<ByteRange> outer_to_inner;
        std::optional<ByteRange> token;
        if (!buffers.empty())
            descend(state->tree->root(), offset, buffers.front(), outer_to_inner, token);

        std::vector<ByteRange> ranges{token.value_or(ByteRange{offset, offset})};
        ranges.insert(ranges.end(), outer_to_inner.rbegin(), outer_to_inner.rend());
        ranges.push_back({0, state->text.size()});
        answer.result.push_back(build_chain(ranges, lines, answer.parents));
    }
    return answer;
}
