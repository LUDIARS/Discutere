// w2-H: src/flow/persona-{pool,descriptor,adopt,cast,import}.ts の移植の確認。
// tests/flow/persona-pool.test.ts / persona-import.test.ts / persona-state.test.ts (buildFlowCast) /
// persona-bridge.test.ts (descriptor) の観点と、Node の golden (fixtures/flow/w2h.json)。
#include "w2h_support.hpp"

#include <algorithm>
#include <set>

#include "base/sqlite.hpp"
#include "base/strings.hpp"
#include "flow/persona_adopt.hpp"
#include "flow/persona_cast.hpp"
#include "flow/persona_descriptor.hpp"
#include "flow/persona_import.hpp"
#include "flow/persona_pool.hpp"
#include "flow/sentiment_vector.hpp"
#include "w1e_fixture.hpp"

using namespace di;
using namespace di::flow;

namespace {

PoolPersona persona_from_json(const Json& j) {
    PoolPersona p;
    p.id = json_string(j, "id");
    p.name = json_string(j, "name");
    p.role = json_string(j, "role");
    p.speechStyle = json_string(j, "speechStyle");
    for (const auto& t : json_array(j, "traits")) p.traits.push_back(t.get<std::string>());
    for (const auto& x : json_array(j, "affectVector")) p.affectVector.push_back(x.get<double>());
    p.origin = json_string(j, "origin");
    p.learningSource = json_opt_string(j, "learningSource");
    p.typicality = json_opt_number(j, "typicality");
    p.polarityBias = json_opt_number(j, "polarityBias");
    p.affectDispersion = json_opt_number(j, "affectDispersion");
    const Json& axes = json_object(j, "preferenceAxes");
    for (auto it = axes.begin(); it != axes.end(); ++it) p.preferenceAxes.emplace_back(it.key(), it.value().get<double>());
    const Json& attrs = json_object(j, "attributes");
    p.attributes.ageBand = json_opt_string(attrs, "ageBand");
    p.attributes.spending = json_opt_string(attrs, "spending");
    for (const auto& a : json_array(j, "aversions")) p.aversions.push_back({json_string(a, "target"), json_number(a, "strength")});
    for (const auto& m : json_array(j, "mechanicReactions"))
        p.mechanicReactions.push_back({json_string(m, "mechanicId"), json_number(m, "sentiment")});
    return p;
}

std::vector<double> zeros() { return std::vector<double>(DIM, 0.0); }

PoolPersona seed(const std::string& id, const std::string& name, std::vector<double> v, const std::string& source = "") {
    PoolPersona p;
    p.id = id;
    p.name = name;
    p.role = "opinion";
    p.speechStyle = "口調";
    p.traits = {"特徴"};
    p.affectVector = std::move(v);
    p.origin = "seed";
    if (!source.empty()) p.learningSource = source;
    return p;
}

std::vector<SpeakerOpinions> speakers_from_json(const Json& arr) {
    std::vector<SpeakerOpinions> out;
    for (const auto& sp : arr) {
        SpeakerOpinions s;
        s.speakerId = json_string(sp, "speakerId");
        s.source = json_opt_string(sp, "source");
        for (const auto& o : json_array(sp, "opinions"))
            s.opinions.push_back({json_string(o, "text"), json_opt_string(o, "gameSlug"), json_opt_number(o, "weight")});
        out.push_back(std::move(s));
    }
    return out;
}

Json evaluated_json(const EvaluateSpeakersResult& r) {
    Json c = Json::array(), rej = Json::array();
    for (const auto& x : r.candidates) c.push_back(to_json(x));
    for (const auto& x : r.rejected) rej.push_back(to_json(x));
    return Json{{"candidates", c}, {"rejected", rej}};
}

} // namespace

TEST_CASE("descriptor: same text as Node for every compartment shape (golden)") {
    for (const auto& c : w2h::fx()["descriptors"])
        CHECK_EQ(build_persona_descriptor(persona_from_json(c["persona"])), c["text"].get<std::string>());
    CHECK_EQ(persona_axis_label("style.explorer"), std::string("探索"));
    CHECK_EQ(persona_axis_label("x.y"), std::string("x.y"));
}

TEST_CASE("evaluate_speakers: adoption rules and #125 features match Node (golden)") {
    const auto& g = w2h::fx()["evaluateSpeakers"];
    const auto speakers = speakers_from_json(g["speakers"]);
    CHECK_EQ(w2h::s(evaluated_json(evaluate_speakers(speakers))), w2h::s(g["default"]));
    CHECK_EQ(w2h::s(evaluated_json(evaluate_speakers(speakers, {3.0, 0.2, 0.0}))), w2h::s(g["loose"]));
}

TEST_CASE("payload parse: same normalisation and error messages as Node (golden)") {
    for (const auto& c : w2h::fx()["payloads"]) {
        if (c["ok"].get<bool>()) {
            CHECK_EQ(w2h::s(to_json(parse_voluptas_persona_payload(c["input"]))), w2h::s(c["payload"]));
        } else {
            try {
                parse_voluptas_persona_payload(c["input"]);
                CHECK(false);
            } catch (const std::invalid_argument& e) {
                CHECK_EQ(std::string(e.what()), c["error"].get<std::string>());
            }
        }
    }
    CHECK_EQ(persona_import_reason_code("unsupported exportSpecVersion: 1"), std::string("export-spec"));
    CHECK_EQ(persona_import_reason_code("x"), std::string("invalid-payload"));
    CHECK_THROWS(parse_voluptas_persona_batch(Json{{"x", 1}}));
    CHECK_EQ(parse_voluptas_persona_batch(Json{{"personas", Json::array({1})}}).size(), 1u);
}

TEST_CASE("pool: insert/get/list/archive, affinity, theme possession, find (persona-pool.test.ts)") {
    w2h::TempFlowDb tmp;
    auto rogue = zeros();
    rogue[0] = 0.9;
    rogue[3] = 1;
    auto gacha = zeros();
    gacha[0] = 0.2;
    gacha[5] = 1;
    insert_pool_persona(seed("p-rogue", "ローグ好き太郎", rogue, "roguelike"));
    insert_pool_persona(seed("p-gacha", "ソシャゲ花子", gacha, "gacha"));
    auto got = get_pool_persona("p-rogue");
    CHECK(got.has_value());
    CHECK_EQ(got->name, std::string("ローグ好き太郎"));
    CHECK_EQ(got->affectVector.size(), DIM);
    CHECK_EQ(list_pool_personas().size(), 2u);
    CHECK_EQ(list_pool_personas({std::nullopt, std::string("gacha")}).size(), 1u);
    archive_pool_persona("p-gacha");
    CHECK_EQ(list_pool_personas().size(), 1u);
    CHECK(!get_pool_persona("p-gacha"));
    insert_pool_persona(seed("p-gacha2", "ソシャゲ花子", gacha, "gacha"));
    CHECK_THROWS(insert_pool_persona(seed("bad", "x", {0.1})));

    auto v = zeros();
    v[0] = 0.95;
    v[3] = 1;
    auto hits = select_by_affinity(v, 1);
    CHECK_EQ(hits.size(), 1u);
    CHECK_EQ(hits[0].persona.id, std::string("p-rogue"));
    CHECK(hits[0].similarity > 0);
    CHECK_EQ(select_by_affinity(v, 2, list_pool_personas()).size(), 2u);
    CHECK(select_by_affinity(v, -1).empty());
    auto fp = to_flow_persona(hits[0].persona, {std::string("opinion"), "claude-haiku-4-5-20251001", false, std::nullopt});
    CHECK_EQ(fp.model, std::string("claude-haiku-4-5-20251001"));
    CHECK_EQ(fp.stance, std::string("opinion"));

    insert_pool_persona(seed("p-themed", "達成感おじさん", text_to_vector("高難度を上達する達成感")));
    CHECK_EQ(select_possession_by_theme("高難度を上達する達成感について", 1)[0].persona.id, std::string("p-themed"));

    CHECK_EQ(find_pool_persona("p-rogue")->id, std::string("p-rogue"));
    CHECK_EQ(find_pool_persona("  ローグ好き太郎 ")->id, std::string("p-rogue"));
    CHECK(!find_pool_persona("存在しない"));
    CHECK(!find_pool_persona("   "));
}

TEST_CASE("adopt_personas: speaker upsert is idempotent and labels are masked") {
    w2h::TempFlowDb tmp;
    const auto speakers = speakers_from_json(w2h::fx()["evaluateSpeakers"]["speakers"]);
    std::vector<SpeakerOpinions> two(speakers.begin(), speakers.begin() + 5);  // persona-pool.test.ts と同じ 4 + 全ネガ 1
    auto res = adopt_personas(two, {10.0, std::nullopt, std::nullopt});
    CHECK_EQ(res.adopted.size(), 1u);
    CHECK_EQ(res.rejected.size(), 4u);
    CHECK(str::starts_with(res.adopted[0].name, "論者#"));
    CHECK_EQ(res.adopted[0].origin, std::string("adopted"));
    CHECK_EQ(*res.adopted[0].sourceSpeakerId, std::string("ext:steam:good"));
    CHECK(res.adopted[0].typicality.has_value());
    CHECK_EQ(list_pool_personas({std::string("adopted"), std::nullopt}).size(), 1u);
    adopt_personas(two, {10.0, std::nullopt, std::nullopt});
    CHECK_EQ(list_pool_personas({std::string("adopted"), std::nullopt}).size(), 1u);
    // 母集団に別の採用者が増えると、既存 adopted の typicality も再計算される
    const auto all = adopt_personas(speakers, {10.0, std::nullopt, std::nullopt});
    CHECK_EQ(list_pool_personas({std::string("adopted"), std::nullopt}).size(), evaluate_speakers(speakers).candidates.size());
    CHECK(all.adopted.size() >= 1);
}

TEST_CASE("build_flow_cast: specified pool persona joins the generated cast (persona-state.test.ts)") {
    w2h::TempFlowDb tmp;
    auto p = seed("p-specified", "指定 太郎", zeros());
    p.origin = "generated";
    p.role = "debater";
    insert_pool_persona(p);
    std::vector<std::string> warns;
    BuildFlowCastArgs a;
    a.count = 4;
    a.defaultModel = "m";
    a.rng = w1e::lcg(13);
    a.personaIds = {" p-specified", "p-specified", "指定 太郎", "", "missing"};
    a.warn = [&](const std::string& m) { warns.push_back(m); };
    const auto cast = build_flow_cast(a);
    CHECK_EQ(warns.size(), 1u);
    CHECK_EQ(cast[0].role, std::string("facilitator"));
    CHECK_EQ(cast.size(), 4u);
    CHECK_EQ(cast[1].id, std::string("p-specified"));
    CHECK_EQ(cast[1].stance, std::string("pro"));
    std::set<std::string> names;
    for (const auto& c : cast) names.insert(c.name);
    CHECK_EQ(names.size(), cast.size());
}

TEST_CASE("import_voluptas_personas: pseudonymous upsert, archive revival, dry run (persona-import.test.ts)") {
    w2h::TempFlowDb tmp;
    const std::string uid = "ext:voluptas:0123456789abcdef";
    const Json vec = Json(std::vector<double>(DIM, 0.25));
    auto first = import_voluptas_personas(Json{{"personas", Json::array({Json{{"user_id", uid}, {"affect_vector", vec},
                                                                               {"vector_spec_version", 1}, {"traits", {"探索型"}}}})}});
    CHECK_EQ(w2h::s(to_json(first)), std::string(R"({"imported":1,"inserted":1,"updated":0,"skipped":0,"skipReasons":{},"dryRun":false})"));
    auto persona = find_pool_persona_by_user_id(uid);
    CHECK(persona.has_value());
    CHECK_EQ(persona->origin, std::string("imported"));
    CHECK(str::starts_with(persona->name, "プレイヤー#"));
    CHECK(!str::contains(persona->name, uid));
    CHECK_EQ(*persona->sourceSpeakerId, "ext:feedback:" + uid);

    const Json full = Json{{"pseudoId", uid},
                           {"affectVector", Json(std::vector<double>(DIM, 0.5))},
                           {"vectorSpecVersion", 1},
                           {"exportSpecVersion", 2},
                           {"traits", {"慎重型"}},
                           {"preferenceAxes", {{"style.explorer", 0.9}, {"style.routine_tolerance", 0.1}}},
                           {"attributes", {{"ageBand", "20s"}, {"spending", "light"}}},
                           {"aversions", Json::array({Json{{"target", "mechanic:core/gacha"}, {"strength", 0.8}}})},
                           {"mechanicReactions", Json::array({Json{{"mechanicId", "action/dodge-roll"}, {"sentiment", 1.5}}})}};
    auto second = import_voluptas_personas(Json::array({full}));
    CHECK_EQ(second.updated, 1.0);
    CHECK_EQ(list_pool_personas({std::string("imported"), std::nullopt}).size(), 1u);
    CHECK_EQ(find_pool_persona_by_user_id(uid)->traits, std::vector<std::string>{"慎重型"});
    CHECK_EQ(w2h::s(preference_axes_to_json(find_pool_persona_by_user_id(uid)->preferenceAxes)),
             std::string(R"({"style.explorer":0.9,"style.routine_tolerance":0.1})"));

    // レビュー採用は Voluptas の profile を置き換えない
    PoolPersona dup = seed("adopted-duplicate", "論者#duplicate", std::vector<double>(DIM, -0.5));
    dup.origin = "adopted";
    dup.speechStyle = "";
    dup.traits = {};
    dup.sourceSpeakerId = "ext:feedback:" + uid;
    dup.typicality = 0.8;
    upsert_pool_persona_by_speaker(dup);
    CHECK_EQ(list_pool_personas().size(), 1u);
    CHECK_EQ(find_pool_persona_by_user_id(uid)->origin, std::string("imported"));
    CHECK_EQ(find_pool_persona_by_user_id(uid)->affectVector[0], 0.5);

    const std::string id = find_pool_persona_by_user_id(uid)->id;
    archive_pool_persona(id);
    CHECK(!find_pool_persona_by_user_id(uid));
    auto revived = import_voluptas_personas(Json::array({full}));
    CHECK_EQ(revived.updated, 1.0);
    CHECK_EQ(find_pool_persona_by_user_id(uid)->id, id);

    auto skipped = import_voluptas_personas(Json::array({Json{{"pseudoId", "raw"}, {"affectVector", vec}, {"vectorSpecVersion", 1}}}),
                                            {false, 2});
    CHECK_EQ(w2h::s(to_json(skipped)), std::string(R"({"imported":0,"inserted":0,"updated":0,"skipped":3,"skipReasons":{"invalid-json":2,"pseudo-id":1},"dryRun":false})"));
    auto dry = import_voluptas_personas(
        Json::array({Json{{"pseudoId", "ext:voluptas:1111111111111111"}, {"affectVector", vec}, {"vectorSpecVersion", 1}}}), {true, 0});
    CHECK_EQ(dry.inserted, 1.0);
    CHECK(!find_pool_persona_by_user_id("ext:voluptas:1111111111111111"));
}

TEST_CASE("boundary: broken / huge DB JSON columns read as empty, never crash") {
    w2h::TempFlowDb tmp;
    insert_pool_persona(seed("p1", "n1", zeros()));
    auto db = db::get_flow_db();
    db->run("UPDATE flow_persona SET traits_json = '{broken', preference_axes_json = '[]', aversions_json = ? WHERE id = 'p1'",
            {"[" + std::string(kPersonaJsonColumnMaxBytes, ' ') + "]"});
    auto p = get_pool_persona("p1");
    CHECK(p.has_value());
    CHECK(p->traits.empty());
    CHECK(p->preferenceAxes.empty());
    CHECK(p->aversions.empty());  // 上限 + 1 以上は読まない
    // 上限ちょうど (空白で埋めた配列) は読める
    std::string exact = "[]";
    exact.append(kPersonaJsonColumnMaxBytes - exact.size(), ' ');
    db->run("UPDATE flow_persona SET aversions_json = ? WHERE id = 'p1'", {exact});
    CHECK(get_pool_persona("p1").has_value());
    // 深い入れ子は拒否 (空扱い)
    db->run("UPDATE flow_persona SET traits_json = ? WHERE id = 'p1'", {std::string(100, '[') + std::string(100, ']')});
    CHECK(get_pool_persona("p1")->traits.empty());
    // 型が違う要素は落とす
    db->run("UPDATE flow_persona SET traits_json = '[\"a\", 1, null, \"b\"]' WHERE id = 'p1'");
    CHECK_EQ(get_pool_persona("p1")->traits, (std::vector<std::string>{"a", "b"}));
}

TEST_CASE("boundary: payload traits at limits and huge batches") {
    const std::string uid = "ext:voluptas:0123456789abcdef";
    const Json vec = Json(std::vector<double>(DIM, 0.25));
    Json traits = Json::array();
    for (int i = 0; i < 1000; ++i) traits.push_back("t" + std::to_string(i));
    auto p = parse_voluptas_persona_payload(Json{{"pseudoId", uid}, {"affectVector", vec}, {"vectorSpecVersion", 1}, {"traits", traits}});
    CHECK_EQ(p.traits.size(), 32u);
    const std::string t80(80, 'x'), t81(81, 'y');
    auto q = parse_voluptas_persona_payload(Json{{"pseudoId", uid}, {"affectVector", vec}, {"vectorSpecVersion", 1}, {"traits", {t80, t81}}});
    CHECK_EQ(q.traits[0].size(), 80u);
    CHECK_EQ(q.traits[1].size(), 80u);
    CHECK_EQ(parse_voluptas_persona_payload(Json{{"pseudoId", uid}, {"affectVector", vec}, {"vectorSpecVersion", 1},
                                                 {"attributes", {{"ageBand", std::string(80, 'a')}}}})
                 .attributes.ageBand->size(),
             80u);
    CHECK_THROWS(parse_voluptas_persona_payload(Json{{"pseudoId", uid}, {"affectVector", vec}, {"vectorSpecVersion", 1},
                                                     {"attributes", {{"ageBand", std::string(81, 'a')}}}}));
    CHECK_THROWS(parse_voluptas_persona_payload(Json{{"pseudoId", uid + "0"}, {"affectVector", vec}, {"vectorSpecVersion", 1}}));
    w2h::TempFlowDb tmp;
    Json many = Json::array();
    for (int i = 0; i < 5000; ++i) many.push_back(Json{{"pseudoId", "nope"}});
    auto r = import_voluptas_personas(many, {true, 0});
    CHECK_EQ(r.skipped, 5000.0);
}

DI_TEST_MAIN()
