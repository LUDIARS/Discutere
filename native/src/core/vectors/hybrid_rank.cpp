#include "core/vectors/hybrid_rank.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <unordered_set>

#include "base/strings.hpp"
#include "core/vectors/vector_math.hpp"

namespace di::core::vectors {
namespace {

/** RRF の順位平滑化定数 (Cormack et al. の慣例値)。 */
constexpr double RRF_K = 60;

using Grams = std::unordered_set<std::uint32_t>;

/** 文字 bigram の集合 (小文字化 + 空白圧縮)。JS と同じく UTF-16 のコード単位 2 つで 1 bigram。 */
Grams bigrams(std::string_view text) {
    const std::string t = str::squish(str::to_lower(text));
    std::vector<std::uint16_t> units;
    units.reserve(t.size());
    for (std::uint32_t cp : str::codepoints(t)) {
        if (cp >= 0x10000) {
            cp -= 0x10000;
            units.push_back(static_cast<std::uint16_t>(0xD800 + (cp >> 10)));
            units.push_back(static_cast<std::uint16_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            units.push_back(static_cast<std::uint16_t>(cp));
        }
    }
    Grams out;
    for (std::size_t i = 0; i + 1 < units.size(); ++i)
        out.insert((static_cast<std::uint32_t>(units[i]) << 16) | units[i + 1]);
    return out;
}

double jaccard(const Grams& a, const Grams& b) {
    if (a.empty() || b.empty()) return 0;
    const Grams& small = a.size() <= b.size() ? a : b;
    const Grams& large = a.size() <= b.size() ? b : a;
    std::size_t inter = 0;
    for (auto g : small)
        if (large.count(g)) ++inter;
    return static_cast<double>(inter) / static_cast<double>(a.size() + b.size() - inter);
}

} // namespace

void FusedScores::add(const std::string& id, double score) {
    auto it = index_.find(id);
    if (it != index_.end()) {
        entries_[it->second].second += score;
        return;
    }
    index_.emplace(id, entries_.size());
    entries_.emplace_back(id, score);
}

std::optional<double> FusedScores::get(std::string_view id) const {
    auto it = index_.find(std::string(id));
    if (it == index_.end()) return std::nullopt;
    return entries_[it->second].second;
}

FusedScores rrf_fuse(const std::vector<std::vector<std::string>>& rankings, const std::vector<double>& weights) {
    FusedScores fused;
    for (std::size_t sys = 0; sys < rankings.size(); ++sys) {
        const double w = sys < weights.size() ? weights[sys] : 1.0;
        const auto& ranking = rankings[sys];
        for (std::size_t i = 0; i < ranking.size(); ++i)
            fused.add(ranking[i], w / (RRF_K + static_cast<double>(i) + 1));
    }
    return fused;
}

std::vector<MmrCandidate> mmr_select(const std::vector<MmrCandidate>& candidates, int k, double lambda) {
    if (k <= 0) return {};
    std::vector<MmrCandidate> remaining = candidates;
    std::stable_sort(remaining.begin(), remaining.end(),
                     [](const MmrCandidate& a, const MmrCandidate& b) { return b.relevance < a.relevance; });
    std::vector<MmrCandidate> selected;
    while (selected.size() < static_cast<std::size_t>(k) && !remaining.empty()) {
        std::size_t best_idx = 0;
        double best_score = -std::numeric_limits<double>::infinity();
        for (std::size_t i = 0; i < remaining.size(); ++i) {
            const auto& c = remaining[i];
            double max_sim = 0;
            for (const auto& s : selected) {
                if (!c.vector || !s.vector) continue;
                const double sim = cosine_similarity(*c.vector, *s.vector);
                if (sim > max_sim) max_sim = sim;
            }
            const double score = lambda * c.relevance - (1 - lambda) * max_sim;
            if (score > best_score) {
                best_score = score;
                best_idx = i;
            }
        }
        selected.push_back(std::move(remaining[best_idx]));
        remaining.erase(remaining.begin() + static_cast<std::ptrdiff_t>(best_idx));
    }
    return selected;
}

double bigram_jaccard_similarity(std::string_view a, std::string_view b) { return jaccard(bigrams(a), bigrams(b)); }

std::vector<std::size_t> dedupe_near_duplicate_indices(const std::vector<std::string_view>& contents, double threshold) {
    std::vector<std::pair<std::size_t, Grams>> kept;
    for (std::size_t i = 0; i < contents.size(); ++i) {
        Grams grams = bigrams(contents[i]);
        bool dup = false;
        for (const auto& k : kept) {
            if (jaccard(k.second, grams) >= threshold) {
                dup = true;
                break;
            }
        }
        if (!dup) kept.emplace_back(i, std::move(grams));
    }
    std::vector<std::size_t> out;
    out.reserve(kept.size());
    for (const auto& k : kept) out.push_back(k.first);
    return out;
}

} // namespace di::core::vectors
