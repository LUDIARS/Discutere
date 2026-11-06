#pragma once
// @ludiars/sentiment-core vocabulary.ts の移植: Affect 統制語彙 (data/vocabulary.json を埋め込み)。
#include <optional>
#include <string>
#include <vector>

#include "base/json.hpp"

namespace di::lib::sentiment_core {

struct AffectEntry {
    std::string key;
    std::string label_ja;
    std::string valence;  // "positive" | "negative" | "ambivalent"
    std::optional<std::string> mda;
    std::string description;
};

/** 語彙の写し (呼び出し側が書き換えても元は変わらない)。 */
std::vector<AffectEntry> load_affect_vocabulary();
std::string affect_vocabulary_version();
Json to_json(const AffectEntry& entry);

} // namespace di::lib::sentiment_core
