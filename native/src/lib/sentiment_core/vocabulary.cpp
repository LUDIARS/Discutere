#include "lib/sentiment_core/vocabulary.hpp"

#include "lib/sentiment_core/lexicon.hpp"

namespace di::lib::sentiment_core {

namespace {

struct Vocabulary {
    std::string version;
    std::vector<AffectEntry> affects;
};

const Vocabulary& vocabulary() {
    static const Vocabulary v = [] {
        const Json doc = parse_json_or_throw(embedded_vocabulary_json(), {1u << 20, 100'000, 16});
        Vocabulary out;
        out.version = json_string(doc, "version");
        for (const auto& e : json_array(doc, "affects")) {
            AffectEntry a;
            a.key = json_string(e, "key");
            a.label_ja = json_string(e, "label_ja");
            a.valence = json_string(e, "valence");
            a.mda = json_opt_string(e, "mda");
            a.description = json_string(e, "description");
            out.affects.push_back(std::move(a));
        }
        return out;
    }();
    return v;
}

} // namespace

std::vector<AffectEntry> load_affect_vocabulary() { return vocabulary().affects; }
std::string affect_vocabulary_version() { return vocabulary().version; }

Json to_json(const AffectEntry& entry) {
    Json o = Json::object();
    o["key"] = entry.key;
    o["label_ja"] = entry.label_ja;
    o["valence"] = entry.valence;
    if (entry.mda) o["mda"] = *entry.mda;
    o["description"] = entry.description;
    return o;
}

} // namespace di::lib::sentiment_core
