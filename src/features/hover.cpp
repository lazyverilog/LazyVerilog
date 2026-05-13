#include "hover.hpp"

std::optional<TextDocumentHover::Result> provide_hover(
    const Analyzer& analyzer, const lsTextDocumentPositionParams& params) {
    const auto& uri = params.textDocument.uri.raw_uri_;
    auto info = analyzer.symbol_at(uri, params.position.line, params.position.character);
    if (!info)
        return std::nullopt;
    if (info->kind == "unknown")
        return std::nullopt;

    std::string kind_label = info->kind;
    auto dot = kind_label.find_last_of('.');
    if (dot != std::string::npos)
        kind_label = kind_label.substr(dot + 1);

    std::string value = "**" + info->name + "**";
    if (!kind_label.empty())
        value += " — *" + kind_label + "*";

    const bool bare_kind =
        info->detail == "module" || info->detail == "function" || info->detail == "task" ||
        info->detail == "typedef";

    std::string body;
    if (!info->doc.empty())
        body = info->doc;
    else if (!info->detail.empty() && !bare_kind)
        body = "```\n" + info->detail + "\n```";

    if (!body.empty())
        value += "\n\n---\n\n" + body;

    MarkupContent mc;
    mc.kind = "markdown";
    mc.value = std::move(value);

    TextDocumentHover::Result result;
    result.contents.first = TextDocumentHover::Left{};
    result.contents.second = optional<MarkupContent>(std::move(mc));
    return result;
}
