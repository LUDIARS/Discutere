#include "lib/sentiment_core/lexicon.hpp"

#include <stdexcept>

#include "base/json.hpp"

namespace di::lib::sentiment_core {

namespace {

std::vector<std::pair<std::string, std::vector<std::string>>> read_groups(const Json& object) {
    std::vector<std::pair<std::string, std::vector<std::string>>> out;
    for (auto it = object.begin(); it != object.end(); ++it) {
        std::vector<std::string> words;
        for (const auto& w : it.value())
            if (w.is_string()) words.push_back(w.get<std::string>());
        out.emplace_back(it.key(), std::move(words));
    }
    return out;
}

SentimentLexicon load() {
    // 埋め込みの辞書 (信頼できる入力) だが、外部入力と同じく上限つきで読む。
    const Json doc = parse_json_or_throw(embedded_lexicon_json(), {1u << 20, 100'000, 16});
    SentimentLexicon lex;
    const Json& polarity = json_object(doc, "polarity");
    for (auto it = polarity.begin(); it != polarity.end(); ++it)
        if (it.value().is_number()) lex.polarity.emplace_back(it.key(), it.value().get<double>());
    lex.emotions = read_groups(json_object(doc, "emotions"));
    lex.aspects = read_groups(json_object(doc, "aspects"));
    for (const auto& w : json_array(doc, "arousal"))
        if (w.is_string()) lex.arousal.push_back(w.get<std::string>());
    return lex;
}

} // namespace

const SentimentLexicon& sentiment_lexicon() {
    static const SentimentLexicon lex = load();
    return lex;
}

} // namespace di::lib::sentiment_core
