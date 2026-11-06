#pragma once
// ハイブリッド検索の順位融合・多様化 (純関数, src/core/vectors/hybrid-rank.ts)。
//   1. rrf_fuse — キーワード順位と埋め込み類似順位を Reciprocal Rank Fusion で融合。
//   2. mmr_select — Maximal Marginal Relevance。関連度と「既選択との非類似」を両立させる。
//   3. dedupe_near_duplicates — 文字 bigram Jaccard の近似重複抑制 (先勝ち)。
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace di::core::vectors {

/** RRF の結果。TS の Map と同じく最初に現れた順を保つ。 */
class FusedScores {
public:
    void add(const std::string& id, double score);
    std::optional<double> get(std::string_view id) const;
    bool has(std::string_view id) const { return get(id).has_value(); }
    const std::vector<std::pair<std::string, double>>& entries() const { return entries_; }

private:
    std::vector<std::pair<std::string, double>> entries_;
    std::unordered_map<std::string, std::size_t> index_;
};

/**
 * Reciprocal Rank Fusion。各ランキング (id 配列、先頭=1位) を融合する。
 * weights は系別の重み (足りない分・省略は 1.0)。
 */
FusedScores rrf_fuse(const std::vector<std::vector<std::string>>& rankings, const std::vector<double>& weights = {});

struct MmrCandidate {
    std::string id;
    /** 融合済み関連度スコア (大きいほど関連)。 */
    double relevance = 0;
    /** 埋め込みベクトル。無い候補は多様性ペナルティ無しで relevance により競合する。 */
    std::optional<std::vector<double>> vector;
};

/** score = λ * relevance - (1-λ) * max(既選択との cosine 類似) で上位 k 件を選ぶ。 */
std::vector<MmrCandidate> mmr_select(const std::vector<MmrCandidate>& candidates, int k, double lambda = 0.7);

/** 2 テキストの文字 bigram Jaccard 類似度 (0..1)。bigram は JS と同じく UTF-16 単位。 */
double bigram_jaccard_similarity(std::string_view a, std::string_view b);

struct DedupeItem {
    std::string id;
    std::string content;
};

/** 近似重複する項目を先勝ちで畳んだ添字 (入力順) を返す。threshold は bigram Jaccard の閾値。 */
std::vector<std::size_t> dedupe_near_duplicate_indices(const std::vector<std::string_view>& contents,
                                                       double threshold = 0.82);

/** dedupeNearDuplicates (id/content を持つ任意の型)。 */
template <class T>
std::vector<T> dedupe_near_duplicates(const std::vector<T>& items, double threshold = 0.82) {
    std::vector<std::string_view> contents;
    contents.reserve(items.size());
    for (const auto& item : items) contents.emplace_back(item.content);
    std::vector<T> out;
    for (std::size_t i : dedupe_near_duplicate_indices(contents, threshold)) out.push_back(items[i]);
    return out;
}

} // namespace di::core::vectors
