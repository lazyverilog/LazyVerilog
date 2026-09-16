#include "position_encoding.hpp"

#include <rapidjson/document.h>

namespace {

// The method name as it appears on the wire.  Used first as a substring test so
// no other message is ever parsed here; `initialize` arrives once per session,
// and this runs on the thread that reads messages.
constexpr const char* kInitialize = "\"initialize\"";

} // namespace

std::optional<PositionEncoding> position_encoding_from_initialize(std::string_view raw_message) {
    if (raw_message.find(kInitialize) == std::string_view::npos)
        return std::nullopt;

    rapidjson::Document doc;
    doc.Parse(raw_message.data(), raw_message.size());
    if (doc.HasParseError() || !doc.IsObject())
        return std::nullopt;

    const auto method = doc.FindMember("method");
    if (method == doc.MemberEnd() || !method->value.IsString() ||
        std::string_view(method->value.GetString(), method->value.GetStringLength()) != "initialize")
        return std::nullopt;

    const auto params = doc.FindMember("params");
    if (params == doc.MemberEnd() || !params->value.IsObject())
        return std::nullopt;
    const auto capabilities = params->value.FindMember("capabilities");
    if (capabilities == params->value.MemberEnd() || !capabilities->value.IsObject())
        return std::nullopt;
    const auto general = capabilities->value.FindMember("general");
    if (general == capabilities->value.MemberEnd() || !general->value.IsObject())
        return std::nullopt;
    const auto encodings = general->value.FindMember("positionEncodings");
    if (encodings == general->value.MemberEnd() || !encodings->value.IsArray())
        return std::nullopt;

    // The array is the client's preference order, and the spec lets the server
    // pick any entry.  Take the first one this server implements rather than the
    // first the client named, because the two agree here: UTF-8 is the only
    // entry worth preferring, and a client that offers it has said it is happy
    // with it.  UTF-32 is deliberately not implemented -- nothing in the
    // document is indexed by code point, so it would be a third conversion with
    // no caller.
    bool offers_utf8 = false;
    bool offers_utf16 = false;
    for (const auto& entry : encodings->value.GetArray()) {
        if (!entry.IsString())
            continue;
        const std::string_view name(entry.GetString(), entry.GetStringLength());
        if (name == "utf-8")
            offers_utf8 = true;
        else if (name == "utf-16")
            offers_utf16 = true;
    }

    if (offers_utf8)
        return PositionEncoding::Utf8;
    if (offers_utf16)
        return PositionEncoding::Utf16;
    // Offered something, but nothing this server speaks.  UTF-16 is the
    // protocol's default and what the reply will withhold a field for, so say
    // nothing rather than claim an encoding neither side meant.
    return std::nullopt;
}
