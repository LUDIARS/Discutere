// w2-H: src/flow/design-gap.ts の移植の確認 (tests/flow/improvement.test.ts / effect-predict.test.ts / vote-lenses.test.ts の該当観点 + golden)。
#include "w2h_support.hpp"

#include "base/fsutil.hpp"
#include "flow/design_gap.hpp"
#include "flow/sentiment_vector.hpp"

using namespace di;
using namespace di::flow;

namespace {

Json vec_json(const std::vector<double>& v) {
    Json a = Json::array();
    for (double x : v) a.push_back(x);
    return a;
}

std::size_t idx(const std::string& d) {
    const auto& dims = vector_dims();
    return static_cast<std::size_t>(std::find(dims.begin(), dims.end(), d) - dims.begin());
}

} // namespace

TEST_CASE("baseline / target / gap are the same vectors as Node (golden)") {
    const auto& g = w2h::fx()["designGap"];
    CHECK_EQ(w2h::s(vec_json(negative_baseline_vector())), w2h::s(g["baseline"]));
    for (const auto& c : g["targets"]) {
        std::vector<MechanicSummary> ms;
        for (const auto& m : c["mechanics"]) ms.push_back(mechanic_summary_from_json(m));
        CHECK_EQ(w2h::s(vec_json(build_target_vector(ms))), w2h::s(c["target"]));
        CHECK_EQ(w2h::s(vec_json(compute_design_gap(negative_baseline_vector(), build_target_vector(ms)))), w2h::s(c["gap"]));
    }
}

TEST_CASE("design gap points to the positive direction (improvement.test.ts)") {
    MechanicSummary m;
    m.name = "テストメカニクス";
    m.intended_valence = "positive";
    m.intended_aspects = std::vector<std::string>{"fun"};
    m.intended_emotions = std::vector<std::string>{"joy"};
    const auto current = negative_baseline_vector();
    const auto target = build_target_vector({m});
    CHECK_EQ(target[idx("asp.fun")], 1.0);
    CHECK_EQ(target[idx("emo.joy")], 1.0);
    const auto gap = compute_design_gap(current, target);
    CHECK_EQ(gap.size(), DIM);
    CHECK(gap[idx("emo.valence")] > 0 && gap[idx("asp.fun")] > 0 && gap[idx("emo.joy")] > 0);
}

TEST_CASE("load_current_vector: alias expansion, missing warn, broken / oversized files (effect-predict.test.ts)") {
    w2h::TempFlowDb tmp;  // 一時ディレクトリだけ使う
    const std::string dir = tmp.file("games");
    fs::ensure_dir(dir);
    std::vector<double> v(DIM, 0.5);
    v[idx("emo.valence")] = 0.9;
    fs::write_file_atomic(fs::join(dir, "モンスターストライク.sentiment.json"),
                          json_stringify(Json{{"game", "Monster Strike"}, {"slug", "monster-strike"}, {"game_vector", vec_json(v)}}));
    fs::write_file_atomic(fs::join(dir, "zz-broken.sentiment.json"), "{not json");
    fs::write_file_atomic(fs::join(dir, "title.md"), "---\ntitle: Genshin Impact (原神)\n---\n本文\n");
    auto by_alias = load_current_vector("モンスト", dir);
    CHECK(by_alias.has_value());
    CHECK_EQ((*by_alias)[idx("emo.valence")], 0.9);
    CHECK(load_current_vector("モンストの周回が苦痛", dir).has_value());
    std::vector<std::string> warns;
    CHECK(!load_current_vector("存在しないゲームXYZ", dir, {[&](const std::string& m) { warns.push_back(m); }}));
    CHECK(!warns.empty() && warns[0].find("見つかりません") != std::string::npos);
    warns.clear();
    CHECK(!load_current_vector("x", tmp.file("nope"), {[&](const std::string& m) { warns.push_back(m); }}));
    CHECK(warns[0].find("存在しません") != std::string::npos);
    // 次元違い・数値でない game_vector は使わない
    fs::write_file_atomic(fs::join(dir, "short.sentiment.json"), R"({"game_vector":[1,2]})");
    CHECK(!load_current_vector("short", dir));
    Json bad = vec_json(v);
    bad[0] = "x";
    fs::write_file_atomic(fs::join(dir, "strs.sentiment.json"), json_stringify(Json{{"game_vector", bad}}));
    CHECK(!load_current_vector("strs", dir));
    // 上限を超えるファイルは読まない
    fs::write_file_atomic(fs::join(dir, "huge.sentiment.json"),
                          json_stringify(Json{{"game_vector", vec_json(v)}, {"pad", std::string(kMaxSentimentFileBytes, 'x')}}));
    CHECK(!load_current_vector("huge", dir));
}

DI_TEST_MAIN()
