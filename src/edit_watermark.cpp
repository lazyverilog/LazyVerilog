#include "edit_watermark.hpp"

#include <rapidjson/document.h>

#include <string_view>

namespace {

// The method name as it appears on the wire.  Used first as a substring test so
// a message that cannot be a didChange is never parsed; a document whose own
// text happens to contain this string only costs one wasted parse, because the
// parsed method is checked properly below.
constexpr const char* kDidChange = "textDocument/didChange";

} // namespace

void EditWatermark::observe(const std::string& raw_message) {
    if (raw_message.find(kDidChange) == std::string::npos)
        return;

    rapidjson::Document doc;
    doc.Parse(raw_message.c_str(), raw_message.size());
    if (doc.HasParseError() || !doc.IsObject())
        return;

    const auto method = doc.FindMember("method");
    if (method == doc.MemberEnd() || !method->value.IsString() ||
        std::string_view(method->value.GetString(), method->value.GetStringLength()) != kDidChange)
        return;

    const auto params = doc.FindMember("params");
    if (params == doc.MemberEnd() || !params->value.IsObject())
        return;
    const auto text_document = params->value.FindMember("textDocument");
    if (text_document == params->value.MemberEnd() || !text_document->value.IsObject())
        return;
    const auto uri = text_document->value.FindMember("uri");
    if (uri == text_document->value.MemberEnd() || !uri->value.IsString())
        return;

    // LspCpp stores an incoming URI verbatim (Reflect(Reader&, lsDocumentUri&)
    // assigns raw_uri_ and nothing else), so the string here is the same one the
    // handlers will key on.
    std::string key(uri->value.GetString(), uri->value.GetStringLength());

    std::lock_guard<std::mutex> lock(mutex_);
    ++counts_[std::move(key)].received;
}

void EditWatermark::on_dispatch(const std::string& uri) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++counts_[uri].dispatched;
}

bool EditWatermark::superseded(const std::string& uri) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = counts_.find(uri);
    return it != counts_.end() && it->second.received > it->second.dispatched;
}

void EditWatermark::forget(const std::string& uri) {
    std::lock_guard<std::mutex> lock(mutex_);
    counts_.erase(uri);
}
