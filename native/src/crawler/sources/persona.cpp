#include "crawler/sources/persona.hpp"

#include "base/crypto.hpp"

namespace di::crawler::sources {

std::string to_speaker_id(std::string_view source, std::string_view author_id) {
    return "ext:" + std::string(source) + ":" + std::string(author_id);
}

std::string masked_persona_label(std::string_view speaker_id) {
    return "論者#" + crypto::sha256_hex(speaker_id).substr(0, 6);
}

std::optional<SourceBadge> source_badge(const std::optional<SourceAttribution>& attribution) {
    if (!attribution) return std::nullopt;
    return SourceBadge{attribution->source, attribution->sourceUrl};
}

Json to_json(const MaskedSpeaker& v) {
    Json o = Json::object();
    o["speaker"] = v.speaker;
    o["source"] = v.source ? Json(*v.source) : Json(nullptr);
    o["sourceUrl"] = v.sourceUrl ? Json(*v.sourceUrl) : Json(nullptr);
    return o;
}

MaskedSpeaker masked_speaker_with_source(std::string_view speaker_id, const std::optional<SourceAttribution>& attribution) {
    auto badge = source_badge(attribution);
    MaskedSpeaker out;
    out.speaker = masked_persona_label(speaker_id);
    if (badge) {
        out.source = badge->source;
        out.sourceUrl = badge->url;
    }
    return out;
}

} // namespace di::crawler::sources
