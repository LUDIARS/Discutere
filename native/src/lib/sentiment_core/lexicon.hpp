#pragma once
// @ludiars/sentiment-core lexicon.ts の移植。
// 辞書 (data/lexicon.json) は lexicon_data.cpp に埋め込んである (実行時にファイルを読まない)。
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace di::lib::sentiment_core {

struct SentimentLexicon {
    /** 単語 → 極性 (-1..1)。JSON の並び順 (Object.entries と同じ)。 */
    std::vector<std::pair<std::string, double>> polarity;
    /** Plutchik 8 感情 → 語彙。 */
    std::vector<std::pair<std::string, std::vector<std::string>>> emotions;
    /** ゲーム評価軸 → 語彙。 */
    std::vector<std::pair<std::string, std::vector<std::string>>> aspects;
    std::vector<std::string> arousal;
};

/** SENTIMENT_LEXICON (初回に埋め込みの JSON を読む)。 */
const SentimentLexicon& sentiment_lexicon();

/** 埋め込んだ data/lexicon.json の原文。 */
std::string_view embedded_lexicon_json();
/** 埋め込んだ data/vocabulary.json の原文。 */
std::string_view embedded_vocabulary_json();

} // namespace di::lib::sentiment_core
