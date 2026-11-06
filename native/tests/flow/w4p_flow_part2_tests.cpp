// w4p_flow のテスト (2/4)。準備は w4p_flow_support.hpp。
#include "w4p_flow_support.hpp"

using namespace w4p_flow_support;

TEST_CASE("golden: pick_side_speaker / ensure_debaters") {
    std::vector<FlowPersona> speakers;
    for (const auto& p : golden()["speakers"]) speakers.push_back(persona_from(p));
    for (const auto& c : golden()["pickSide"]) {
        std::map<std::string, int> counts;
        for (const auto& [k, v] : c["counts"].items()) counts[k] = v;
        const auto* p = dialectic::pick_side_speaker(speakers, c["side"].get<std::string>(), counts);
        W3L_SAME(p ? Json(p->id) : Json(nullptr), c["picked"]);
    }
    for (const auto& c : golden()["ensureDebaters"]) {
        std::vector<FlowPersona> cast;
        for (const auto& p : c["cast"]) cast.push_back(persona_from(p));
        std::vector<std::string> warns;
        if (c["ok"].get<bool>()) {
            const auto r = dialectic::ensure_debaters(cast, [&](const std::string& m) { warns.push_back(m); });
            W3L_SAME(to_json(r.pro), c["pro"]);
            W3L_SAME(to_json(r.con), c["con"]);
            Json result = Json::array();
            for (const auto& p : r.cast) result.push_back(to_json(p));
            W3L_SAME(result, c["result"]);
        } else {
            try {
                (void)dialectic::ensure_debaters(cast, [&](const std::string& m) { warns.push_back(m); });
                throw test::Failure("ensure_debaters should throw");
            } catch (const std::runtime_error& e) {
                CHECK_EQ(std::string(e.what()), c["error"].get<std::string>());
            }
        }
        W3L_SAME(Json(warns), c["warns"]);
    }
}

TEST_CASE("golden: parse_flow_kind (entrypoints.test.ts / forum-flow-tags.test.ts)") {
    for (const auto& c : golden()["flowKinds"]) {
        const auto k = parse_flow_kind(std::string_view(c["label"].get_ref<const std::string&>()));
        W3L_SAME(k ? Json(*k) : Json(nullptr), c["kind"]);
    }
    CHECK(!parse_flow_kind(std::nullopt));
}

TEST_CASE("golden: 未来予測の話者割当・往復・段階・投稿の分割") {
    for (const auto& c : golden()["casts"]) {
        std::vector<FlowUtteranceRecord> us;
        for (const auto& u : c["utterances"]) us.push_back(record_from(u));
        const auto cast = foresight::build_foresight_cast(us);
        W3L_SAME(cast ? foresight::to_json(*cast) : Json(nullptr), c["cast"]);
    }
    for (const auto& c : golden()["normalizeTurns"]) CHECK_EQ(foresight::normalize_camp_turns(c["n"].get<double>()), c["v"].get<int>());
    CHECK_EQ(foresight::normalize_camp_turns(std::nan("")), golden()["normalizeTurnsSpecial"]["nan"].get<int>());
    CHECK_EQ(foresight::normalize_camp_turns(INFINITY), golden()["normalizeTurnsSpecial"]["inf"].get<int>());
    for (const auto& c : golden()["stages"]) CHECK_EQ(foresight::camp_stage_at(c["i"].get<int>()), c["stage"].get<std::string>());
    std::vector<FlowUtteranceRecord> main_utterances;
    for (const auto& u : golden()["casts"][0]["utterances"]) main_utterances.push_back(record_from(u));
    const auto cast = foresight::build_foresight_cast(main_utterances);
    for (const auto& c : golden()["turnOrders"]) {
        Json ids = Json::array();
        for (const auto& s : foresight::build_camp_turn_order(cast->speakers, c["turns"].get<double>())) ids.push_back(s.personaId);
        W3L_SAME(ids, c["order"]);
    }
    std::vector<foresight::ForesightSpeaker> reform_only;
    for (const auto& s : cast->speakers)
        if (s.camp == "reform") reform_only.push_back(s);
    CHECK_EQ(foresight::build_camp_turn_order(reform_only, 4).size(), golden()["turnOrderOneCamp"].get<std::size_t>());
    for (const auto& c : golden()["splits"])
        W3L_SAME(Json(foresight::split_for_post(c["text"].get<std::string>(), c["limit"].get<std::size_t>())), c["chunks"]);
    for (const auto& c : golden()["labels"]) W3L_SAME(Json(foresight::label_parts(strings(c["chunks"]))), c["labeled"]);
}

// ─── dispatch の振り分け (注入ドライバ) ─────────────────────────────

TEST_CASE("dispatch: ラベルごとに正しいドライバ / flow 必須 (entrypoints.test.ts)") {
    w3l::TempFlowDb tmp;
    set_env(Json::object());
    std::vector<std::string> calls;
    w3l::FnLLM llm([](const persona_engine::LLMInvokeArgs&) { return persona_engine::LLMResult::success(""); });
    DispatchDeps d;
    d.llm = borrow(llm);
    d.drivers.discussion = [&](persona_engine::LLMClient&, const std::string&, const std::vector<FlowTag>&, const FlowDirectorDeps&) {
        calls.push_back("discussion");
        return fake_result("s");
    };
    d.drivers.improvement = [&](persona_engine::LLMClient&, const std::string&, const std::vector<FlowTag>&, const FlowDirectorDeps&) {
        calls.push_back("improvement");
        return fake_result("s");
    };
    auto in = [](std::string flow) {
        DispatchInput i;
        i.theme = "t";
        i.flow = std::move(flow);
        return i;
    };
    CHECK_EQ(dispatch_flow(in("議論"), d).kind, std::string("discussion"));
    CHECK_EQ(dispatch_flow(in("改善"), d).kind, std::string("improvement"));
    CHECK_EQ(calls, (std::vector<std::string>{"discussion", "improvement"}));
    for (const char* bad : {"学習", "壁打ち", "sparring", "", "雑談"}) {
        try {
            (void)dispatch_flow(in(bad), d);
            throw test::Failure(std::string("should reject: ") + bad);
        } catch (const FlowTypeRequiredError&) {
        }
    }
    // llm が無い呼び出しは受けない
    DispatchDeps none;
    CHECK_THROWS(dispatch_flow(in("議論"), none));
    // rounds / turnsPerRound / personaIds / state がドライバに渡る
    auto state = std::make_shared<FlowRunState>();
    d.state = state;
    d.drivers.discussion = [&](persona_engine::LLMClient&, const std::string&, const std::vector<FlowTag>&, const FlowDirectorDeps& fd) {
        CHECK(fd.state == state);
        CHECK(fd.rounds && *fd.rounds == 3);
        CHECK(fd.personaIds && fd.personaIds->size() == 1);
        return fake_result("s");
    };
    DispatchInput i = in("企画/議論");
    i.rounds = 3;
    i.personaIds = std::vector<std::string>{"p1"};
    (void)dispatch_flow(i, d);
}

int main() { return di::test::run_all(); }
