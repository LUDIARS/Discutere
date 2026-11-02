// ハイブリッド検索の順位融合・多様化 (tests/core/hybrid-rank.test.ts) + golden (Node の出力)。
#include "core/test_util.hpp"

#include <cmath>

#include "core/vectors/hybrid_rank.hpp"
#include "core/vectors/vector_math.hpp"

using namespace di;
using namespace di::core::vectors;

namespace {
std::vector<std::string> strings(const Json& a) {
    std::vector<std::string> out;
    for (const auto& s : a) out.push_back(s.get<std::string>());
    return out;
}
std::vector<double> doubles(const Json& a) {
    std::vector<double> out;
    for (const auto& s : a) out.push_back(s.get<double>());
    return out;
}
bool close(double a, double b) { return std::fabs(a - b) <= 1e-12 * std::max(1.0, std::fabs(b)); }
} // namespace

TEST_CASE("rrfFuse (TS assertions)") {
    auto fused = rrf_fuse({{"a", "b", "c"}, {"b", "a", "d"}});
    // TS テストは b > a を期待するが、Node 実装でも a と b は同点 (1/61 + 1/62) になり TS テスト自体が落ちる。
    // C++ は Node 実装の値 (golden) に合わせる。
    CHECK_EQ(*fused.get("b"), *fused.get("a"));
    CHECK(*fused.get("a") > *fused.get("c"));
    CHECK(fused.has("d"));
    auto weighted = rrf_fuse({{"a", "b"}, {"b", "a"}}, {0, 1});
    CHECK(*weighted.get("b") > *weighted.get("a"));
}

TEST_CASE("mmrSelect (TS assertions)") {
    auto picked = mmr_select({{"v1", 1.0, std::vector<double>{1, 0}}, {"v2", 0.9, std::vector<double>{0.999, 0.01}},
                              {"v3", 0.5, std::vector<double>{0, 1}}},
                             2, 0.5);
    CHECK_EQ(picked.size(), 2u);
    CHECK_EQ(picked[0].id, std::string("v1"));
    CHECK_EQ(picked[1].id, std::string("v3"));
    auto mixed = mmr_select({{"v1", 1.0, std::vector<double>{1, 0}}, {"n1", 0.8, std::nullopt}, {"n2", 0.2, std::nullopt}}, 2);
    CHECK_EQ(mixed[1].id, std::string("n1"));
    CHECK(mmr_select({}, 3).empty());
    CHECK(mmr_select({{"a", 1, std::vector<double>{1}}}, 0).empty());
}

TEST_CASE("dedupeNearDuplicates (TS assertions)") {
    std::vector<DedupeItem> items = {{"a", "このゲームの引っ張りアクションが最高に気持ちいい"},
                                     {"b", "このゲームの引っ張りアクションが最高に気持ちいい!"},
                                     {"c", "ガチャの排出率が渋すぎて課金する気になれない"}};
    auto kept = dedupe_near_duplicates(items);
    CHECK_EQ(kept.size(), 2u);
    CHECK_EQ(kept[1].id, std::string("c"));
    CHECK_EQ(dedupe_near_duplicates(items, 0.999).size(), 3u);
    CHECK_EQ(dedupe_near_duplicates(std::vector<DedupeItem>{{"x", ""}, {"y", "あ"}}).size(), 2u);
}

TEST_CASE("golden: rrf / mmr / dedupe / jaccard / cosine / isEmbeddingVector") {
    const Json fx = test::load_fixture("core/hybrid_rank.json");
    for (const auto& c : fx["rrf"]) {
        std::vector<std::vector<std::string>> rankings;
        for (const auto& r : c["rankings"]) rankings.push_back(strings(r));
        const auto fused = rrf_fuse(rankings, c.contains("weights") ? doubles(c["weights"]) : std::vector<double>{});
        CHECK_EQ(fused.entries().size(), c["out"].size());
        for (std::size_t i = 0; i < fused.entries().size(); ++i) {
            CHECK_EQ(fused.entries()[i].first, c["out"][i][0].get<std::string>());
            CHECK(close(fused.entries()[i].second, c["out"][i][1].get<double>()));
        }
    }
    for (const auto& c : fx["mmr"]) {
        std::vector<MmrCandidate> cands;
        for (const auto& x : c["candidates"])
            cands.push_back({x["id"].get<std::string>(), x["relevance"].get<double>(),
                             x.contains("vector") ? std::optional<std::vector<double>>(doubles(x["vector"])) : std::nullopt});
        const auto out = c.contains("lambda") ? mmr_select(cands, c["k"].get<int>(), c["lambda"].get<double>())
                                              : mmr_select(cands, c["k"].get<int>());
        CHECK_EQ(out.size(), c["out"].size());
        for (std::size_t i = 0; i < out.size(); ++i) CHECK_EQ(out[i].id, c["out"][i].get<std::string>());
    }
    std::vector<DedupeItem> items;
    for (const auto& x : fx["dedupeItems"]) items.push_back({x["id"].get<std::string>(), x["content"].get<std::string>()});
    for (const auto& c : fx["dedupe"]) {
        const auto out = dedupe_near_duplicates(items, c["threshold"].get<double>());
        CHECK_EQ(out.size(), c["out"].size());
        for (std::size_t i = 0; i < out.size(); ++i) CHECK_EQ(out[i].id, c["out"][i].get<std::string>());
    }
    for (const auto& c : fx["jaccard"])
        CHECK(close(bigram_jaccard_similarity(c["a"].get<std::string>(), c["b"].get<std::string>()), c["out"].get<double>()));
    for (const auto& c : fx["cosine"]) CHECK_EQ(cosine_similarity(doubles(c["a"]), doubles(c["b"])), c["out"].get<double>());
    for (const auto& c : fx["isVector"]) CHECK_EQ(is_embedding_vector(c["v"]), c["out"].get<bool>());
}

TEST_CASE("boundaries: large inputs stay linear-ish and empty inputs are safe") {
    std::vector<std::string> big;
    for (int i = 0; i < 20000; ++i) big.push_back("id" + std::to_string(i));
    auto fused = rrf_fuse({big, big});
    CHECK_EQ(fused.entries().size(), 20000u);
    CHECK(close(*fused.get("id0"), 2.0 / 61));
    std::string huge(1u << 20, 'a');
    CHECK_EQ(bigram_jaccard_similarity(huge, huge), 1.0);
    CHECK_EQ(cosine_similarity({}, {}), 0.0);
}

DI_TEST_MAIN()
