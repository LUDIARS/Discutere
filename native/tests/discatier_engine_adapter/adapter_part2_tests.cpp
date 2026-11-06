// adapter のテスト (2/3)。準備は adapter_support.hpp。
#include "adapter_support.hpp"

using namespace adapter_support;

TEST_CASE("human-utterance.test.ts: 人間の発言を議論 session にミラーし HumanUtterance、ミラーは再発火しない、closed は無視") {
    w3n::TempDir dir;
    w3n::TestConfig cfg(dir, false);
    auto core = core::create_core(dir.file("db.kuzu"), dir.file("events.jsonl"));
    const std::string scene = "discord:g1/c1";
    const auto discord_session = make_session(*core, "discord-session:g1:c1", scene);
    const auto gap_id = make_gap(*core, "面白さの本質");
    const auto discussion = make_session(*core, "discussion-of-gap:" + gap_id, scene);
    StubEngine stub;
    auto bridge = create_event_bridge(*core, stub.bridge(), {ws, 60'000, std::nullopt, {}});
    tick();
    make_utt(*core, discord_session, "u1", "もっと否定的な意見も聞きたい");
    bridge->poll_once();
    CHECK_EQ(stub.count("HumanUtterance"), std::size_t{1});
    {
        std::lock_guard l(stub.m);
        for (const auto& f : stub.fired)
            if (f.kind == "HumanUtterance") {
                CHECK_EQ(f.sessionId.value_or(""), discussion);
                CHECK_EQ((*f.payload)["sessionId"].get<std::string>(), discussion);
                CHECK_EQ((*f.payload)["humanSpeakerId"].get<std::string>(), std::string("u1"));
            }
    }
    const auto mirrored = core->raw().all("SELECT raw_content FROM utterances WHERE session_id = ? AND speaker_id = 'u1'", {discussion});
    CHECK_EQ(mirrored.size(), std::size_t{1});
    CHECK_EQ(mirrored[0].str("raw_content"), std::string("もっと否定的な意見も聞きたい"));

    { std::lock_guard l(stub.m); stub.fired.clear(); }
    bridge->poll_once();
    CHECK_EQ(stub.count("HumanUtterance"), std::size_t{0});

    core::repositories::DesignGapPatch patch;
    patch.workspaceId = ws;
    patch.title = "面白さの本質";
    patch.status = "closed";
    core->repos.designGap.update(gap_id, patch);
    tick();
    { std::lock_guard l(stub.m); stub.fired.clear(); }
    make_utt(*core, discord_session, "u1", "まだ話したい");
    bridge->poll_once();
    CHECK_EQ(stub.count("HumanUtterance"), std::size_t{0});
    // persona / ext の発言・空白だけの発言はミラーしない。
    bridge->stop();
    core->close();
}

TEST_CASE("event-bridge.test.ts: DesignGapDetected → propose、gap session の自動作成と再利用、終結で reset、別 workspace は無視") {
    w3n::TempDir dir;
    w3n::TestConfig cfg(dir, false);
    auto core = core::create_core(dir.file("db.kuzu"), dir.file("events.jsonl"));
    auto pe_db = sql::Database::open(dir.file("pe.db"));
    pe::db::apply_persona_engine_migrations(*pe_db);
    auto adapter = create_discatier_context_provider(*core);
    auto engine = make_engine(pe_db, adapter,
                              {R"({"action":"propose_hypothesis","statement":"first proposal","reasoning":"r"})",
                               R"({"action":"propose_hypothesis","statement":"second proposal","reasoning":"r2"})"});
    std::vector<std::string> errors;
    EventBridgeOptions bo;
    bo.workspaceId = ws;
    bo.pollMs = 60'000;
    bo.onError = [&](const std::string& e, const DiscatierEventRow*) { errors.push_back(e); };
    auto bridge = create_event_bridge(*core, bridge_engine_of(engine), bo);
    const double initial = bridge->last_seen_at();
    tick();
    const auto gap_id = make_gap(*core, "オンボーディング離脱");
    CHECK(bridge->poll_once() >= 1);
    CHECK(bridge->last_seen_at() >= initial);

    const auto hyps = core->raw().all("SELECT id, statement, design_gap_id FROM hypotheses WHERE workspace_id = ?", {ws});
    CHECK(!hyps.empty());
    CHECK_EQ(hyps[0].str("statement"), std::string("first proposal"));
    CHECK_EQ(hyps[0].str("design_gap_id"), gap_id);
    const auto gs = core->raw().get("SELECT scene FROM sessions WHERE workspace_id = ? AND title = ?", {ws, "discussion-of-gap:" + gap_id});
    CHECK(gs.has_value());
    CHECK_EQ(gs->str("scene"), "gap:" + gap_id);

    core::repositories::DesignGapPatch patch;
    patch.workspaceId = ws;
    patch.title = "オンボーディング離脱 (改名)";
    patch.status = "open";
    core->repos.designGap.update(gap_id, patch);
    bridge->poll_once();
    CHECK_EQ(core->raw().all("SELECT id FROM sessions WHERE workspace_id = ? AND title = ?", {ws, "discussion-of-gap:" + gap_id}).size(),
             std::size_t{1});

    Json payload{{"id", hyps[0].str("id")}, {"workspaceId", ws}, {"statement", "first proposal"}, {"status", "integrated"},
                 {"integrated", true}, {"validatedByEmotion", true}, {"designGapId", gap_id}, {"refs", Json::array()}};
    // TS と同じく created_at (ms) > lastSeen で拾うので、直前の event と同じ ms に書くと拾われない
    // (TS からの既知の制約)。Release ビルドは速く同じ ms に入りうるので、ここでも時刻を進める。
    tick();
    core->eventLog.append_event({std::nullopt, "HypothesisIntegrated", payload, std::nullopt});
    CHECK(bridge->poll_once() >= 1);

    const auto before = core->raw().get("SELECT COUNT(*) AS n FROM hypotheses WHERE workspace_id = ?", {ws})->i64("n");
    tick();
    make_gap(*core, "別 workspace gap", std::string("open"), "other-ws");
    bridge->poll_once();
    CHECK_EQ(core->raw().get("SELECT COUNT(*) AS n FROM hypotheses WHERE workspace_id = ?", {ws})->i64("n"), before);
    bridge->stop();
    engine->stop();
    core->close();
}

TEST_CASE("event-bridge-discord.test.ts: gap の evidence から discord scene を継承し、callback に designGapId が届く") {
    w3n::TempDir dir;
    w3n::TestConfig cfg(dir, false);
    auto core = core::create_core(dir.file("db.kuzu"), dir.file("events.jsonl"));
    auto pe_db = sql::Database::open(dir.file("pe.db"));
    pe::db::apply_persona_engine_migrations(*pe_db);
    const auto source_session = make_session(*core, "discord-session:guild-1:channel-1", "discord:guild-1/channel-1");
    const auto utterance_id = make_utt(*core, source_session, "u1", "ヴァンサバの面白いところどこだろう");
    std::optional<std::string> posted_gap;
    DiscatierAdapterOptions opt;
    opt.onPostedHypothesis = [&](const PostedHypothesis& p) { posted_gap = p.designGapId; };
    auto engine = make_engine(pe_db, create_discatier_context_provider(*core, opt),
                              {R"({"action":"propose_hypothesis","statement":"discord proposal","reasoning":"r"})"});
    auto bridge = create_event_bridge(*core, bridge_engine_of(engine), {ws, 60'000, std::nullopt, {}});
    tick();
    const auto gap_id = make_gap(*core, "ヴァンサバの面白さの本質");
    Json evidence{{"source", "discord:auto-classifier"}, {"category", "game_design_question"}, {"utteranceIds", Json::array({utterance_id})},
                  {"guildId", "guild-1"}, {"channelId", "channel-1"}, {"sessionId", source_session}, {"authorId", "u1"}};
    core->raw().run("UPDATE design_gaps SET evidence_json = ?, updated_at = ? WHERE id = ?", {json_stringify(evidence), time::now_ms(), gap_id});
    CHECK(bridge->poll_once() >= 1);
    const auto hyp = core->raw().get("SELECT statement, design_gap_id FROM hypotheses WHERE workspace_id = ? ORDER BY created_at DESC LIMIT 1", {ws});
    CHECK_EQ(hyp->str("statement"), std::string("discord proposal"));
    CHECK_EQ(hyp->str("design_gap_id"), gap_id);
    CHECK_EQ(posted_gap.value_or(""), gap_id);
    CHECK_EQ(core->raw().get("SELECT scene FROM sessions WHERE workspace_id = ? AND title = ?", {ws, "discussion-of-gap:" + gap_id})->str("scene"),
             std::string("discord:guild-1/channel-1"));
    bridge->stop();
    engine->stop();
    core->close();
}

DI_TEST_MAIN()
