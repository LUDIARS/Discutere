// w3-N: DiscussionContextProvider (adapter) と event-bridge。
// tests/persona-engine/{discatier-adapter,find-discussion-session,auto-export,event-bridge,event-bridge-discord,human-utterance}.test.ts の移植 + 境界。
#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <thread>

#include "check.hpp"
#include "w3n_support.hpp"

#include "base/fsutil.hpp"
#include "base/strings.hpp"
#include "base/time.hpp"
#include "discatier_engine_adapter/event_bridge.hpp"
#include "discatier_engine_adapter/index.hpp"
#include "persona_engine/db/migrations.hpp"
#include "persona_engine/index.hpp"
#include "persona_engine/llm/mock.hpp"
#include "persona_engine/seeds/personas.hpp"
#include "persona_engine/seeds/rules.hpp"

using namespace di;
using namespace di::discatier_engine_adapter;
namespace pe = di::persona_engine;

namespace {

const std::string ws = "knowledge";

std::string make_session(core::Core& core, const std::string& title, std::optional<std::string> scene = std::nullopt) {
    core::repositories::SessionInput s;
    s.workspaceId = ws;
    s.title = title;
    s.startedAt = static_cast<double>(time::now_ms());
    s.scene = std::move(scene);
    return core.repos.session.create(s);
}

std::string make_gap(core::Core& core, const std::string& title, std::optional<std::string> status = std::string("open"),
                     const std::string& workspace = ws) {
    core::repositories::DesignGapInput g;
    g.workspaceId = workspace;
    g.title = title;
    g.status = std::move(status);
    return core.repos.designGap.create(g);
}

std::string make_utt(core::Core& core, const std::string& session, const std::string& speaker, const std::string& text) {
    core::repositories::UtteranceInput u;
    u.workspaceId = ws;
    u.sessionId = session;
    u.speakerId = speaker;
    u.rawContent = text;
    u.postedAt = static_cast<double>(time::now_ms());
    return core.repos.utterance.create(u);
}

struct Fired {
    std::string kind;
    std::optional<std::string> sessionId;
    std::optional<Json> payload;
};

struct StubEngine {
    std::mutex m;
    std::vector<Fired> fired;
    std::vector<std::string> resets;
    BridgeEngine bridge() {
        BridgeEngine b;
        b.fireEvent = [this](const pe::engine::EngineEvent& e) {
            std::lock_guard l(m);
            fired.push_back({e.kind, e.sessionId, e.payload});
        };
        b.resetSession = [this](const std::string& s) {
            std::lock_guard l(m);
            resets.push_back(s);
        };
        return b;
    }
    std::size_t count(const std::string& kind) {
        std::lock_guard l(m);
        std::size_t n = 0;
        for (const auto& f : fired) n += f.kind == kind;
        return n;
    }
};

/** bridge は構築時の最新 event を起点にする。同一 ms 衝突を避けて時刻を進める。 */
void tick() { std::this_thread::sleep_for(std::chrono::milliseconds(5)); }

std::shared_ptr<pe::PersonaEngineHandle> make_engine(const sql::DatabasePtr& db, std::shared_ptr<pe::DiscussionContextProvider> ctx,
                                                     std::vector<std::string> answers) {
    std::vector<pe::MockResponder> rs;
    for (auto& a : answers) rs.push_back([a](const pe::LLMInvokeArgs&) -> std::variant<std::string, pe::LLMResult> { return a; });
    pe::PersonaEngineOptions o;
    o.db = db;
    o.llm = std::make_shared<pe::MockLLMClient>(rs);
    o.contextProvider = std::move(ctx);
    o.workspaceId = ws;
    o.personaSeeds = pe::seeds::DISCUSSION_PERSONA_SEEDS();
    o.ruleSeeds = pe::seeds::DISCUSSION_RULE_SEEDS();
    o.tickMs = 60'000;
    o.enableLlm = true;
    auto engine = pe::create_persona_engine(o);
    db->run("UPDATE rules SET cooldown_sec = 0");
    return engine;
}

} // namespace

TEST_CASE("discatier-adapter.test.ts: gaps / propose / active hypotheses / post utterance / defaultSessionId") {
    w3n::TempDir dir;
    w3n::TestConfig cfg(dir, false);
    auto core = core::create_core(dir.file("db.kuzu"), dir.file("events.jsonl"));
    const auto gap_id = make_gap(*core, "オンボーディング離脱");
    const auto session_id = make_session(*core, "design discussion");
    DiscatierAdapterOptions opt;
    opt.defaultSessionId = session_id;
    auto ctx = create_discatier_context_provider(*core, opt);

    auto gaps = ctx->list_recent_gaps(ws, 10);
    CHECK_EQ(gaps.size(), std::size_t{1});
    CHECK_EQ(gaps[0].id, gap_id);
    CHECK_EQ(gaps[0].title, std::string("オンボーディング離脱"));
    CHECK(gaps[0].hypothesisIds.empty());

    const auto hid = ctx->propose_hypothesis({ws, "選択肢を 2 つに絞る", gap_id, "advocate", std::nullopt});
    CHECK(!hid.empty());
    const auto hyps = core->raw().all("SELECT id, statement, design_gap_id FROM hypotheses WHERE workspace_id = ?", {ws});
    CHECK_EQ(hyps.size(), std::size_t{1});
    CHECK_EQ(hyps[0].str("statement"), std::string("選択肢を 2 つに絞る"));
    CHECK_EQ(hyps[0].str("design_gap_id"), gap_id);

    const auto active = ctx->list_active_hypotheses(ws, 10);
    CHECK_EQ(active.size(), std::size_t{1});
    CHECK_EQ(active[0].designGapId.value_or(""), gap_id);
    CHECK_EQ(ctx->list_recent_gaps(ws, 10)[0].hypothesisIds, std::vector<std::string>{hid});

    // closed gap の仮説は active から外す。
    core->raw().run("UPDATE design_gaps SET status = 'closed' WHERE id = ?", {gap_id});
    CHECK(ctx->list_active_hypotheses(ws, 10).empty());

    const auto utt = ctx->post_utterance({ws, session_id, "そもそも 2 択でいいのか", "sceptic", std::nullopt});
    const auto utts = ctx->list_recent_utterances(ws, session_id, 10);
    CHECK_EQ(utts.size(), std::size_t{1});
    CHECK_EQ(utts[0].rawContent, std::string("そもそも 2 択でいいのか"));
    CHECK_EQ(core->raw().get("SELECT speaker_id FROM utterances WHERE id = ?", {utt})->str("speaker_id"), std::string("persona:sceptic"));
    CHECK(!ctx->post_utterance({ws, "", "default session fallback", "advocate", std::nullopt}).empty());
    // defaultSessionId も sessionId も無ければ投げる。
    auto bare = create_discatier_context_provider(*core);
    CHECK_THROWS(bare->post_utterance({ws, "", "x", "advocate", std::nullopt}));
    core->close();
}

TEST_CASE("find-discussion-session.test.ts: gap / hypothesis から解決・ended / closed は対象外") {
    w3n::TempDir dir;
    w3n::TestConfig cfg(dir, false);
    auto core = core::create_core(dir.file("db.kuzu"), dir.file("events.jsonl"));
    auto adapter = create_discatier_context_provider(*core);
    const auto gap_id = make_gap(*core, "面白さの本質", std::nullopt);
    const auto session_id = make_session(*core, "discussion-of-gap:" + gap_id);
    core::repositories::HypothesisInput h;
    h.workspaceId = ws;
    h.designGapId = gap_id;
    h.statement = "自動攻撃が手軽さを生む";
    const auto hid = core->repos.hypothesis.create(h);

    CHECK_EQ(adapter->find_discussion_session({ws, std::nullopt, gap_id}).value_or(""), session_id);
    CHECK_EQ(adapter->find_discussion_session({ws, hid, std::nullopt}).value_or(""), session_id);
    CHECK(!adapter->find_discussion_session({ws, std::string("nope"), std::nullopt}));
    CHECK(!adapter->find_discussion_session({ws, std::nullopt, std::string("nope")}));
    core->raw().run("UPDATE sessions SET ended_at = ? WHERE id = ?", {time::now_ms(), session_id});
    CHECK(!adapter->find_discussion_session({ws, std::nullopt, gap_id}));

    const auto gap2 = make_gap(*core, "進行中の議題", std::nullopt);
    const auto active = make_session(*core, "discussion-of-gap:" + gap2, "discord:g1/c1");
    CHECK_EQ(adapter->find_active_discussion_session(ws).value_or(""), active);
    core->raw().run("UPDATE design_gaps SET status = 'closed' WHERE id = ?", {gap2});
    CHECK(!adapter->find_active_discussion_session(ws));
    // converged gap の議論にも着地させない。
    const auto gap3 = make_gap(*core, "収束済み", std::string("converged"));
    make_session(*core, "discussion-of-gap:" + gap3);
    CHECK(!adapter->find_discussion_session({ws, std::nullopt, gap3}));
    core->close();
}

TEST_CASE("auto-export.test.ts: autoExportRootDir で hypothesis / utterance の md を書く・未指定なら書かない・callback の例外は止めない") {
    w3n::TempDir dir;
    w3n::TestConfig cfg(dir, false);
    auto core = core::create_core(dir.file("db.kuzu"), dir.file("events.jsonl"));
    const auto session_id = make_session(*core, "auto-export session");
    const std::string root = dir.file("discussions");
    std::vector<std::string> errors;
    std::vector<PostedHypothesis> posted;
    DiscatierAdapterOptions opt;
    opt.defaultSessionId = session_id;
    opt.autoExportRootDir = root;
    opt.onAutoExportError = [&](const std::string& e, const std::string& type, const std::string&) { errors.push_back(type + ":" + e); };
    opt.onPostedHypothesis = [&](const PostedHypothesis& p) { posted.push_back(p); };
    opt.onPostedUtterance = [](const PostedUtterance&) { throw std::runtime_error("discord down"); };
    auto adapter = create_discatier_context_provider(*core, opt);

    const auto hid = adapter->propose_hypothesis({ws, "auto-exported hypothesis", std::nullopt, "advocate", std::nullopt});
    const auto hyp_md = fs::read_file(root + "/" + ws + "/hypothesis/" + hid + ".md");
    CHECK(hyp_md && str::contains(*hyp_md, "auto-exported hypothesis"));
    CHECK_EQ(posted.size(), std::size_t{1});
    CHECK(!posted[0].designGapId);
    CHECK_EQ(posted[0].hypothesisId, hid);

    const auto uid = adapter->post_utterance({ws, session_id, "auto-exported utterance", "sceptic", std::nullopt});
    const auto utt_md = fs::read_file(root + "/" + ws + "/utterance/" + uid + ".md");
    CHECK(utt_md && str::contains(*utt_md, "auto-exported utterance"));
    CHECK_EQ(errors.size(), std::size_t{1});
    CHECK(str::contains(errors[0], "utt:discord down"));

    std::size_t before = 0;
    for (const auto& e : std::filesystem::recursive_directory_iterator(root)) before += e.is_regular_file();
    DiscatierAdapterOptions opt2;
    opt2.defaultSessionId = session_id;
    create_discatier_context_provider(*core, opt2)->propose_hypothesis({ws, "no auto-export", std::nullopt, "refiner", std::nullopt});
    std::size_t after = 0;
    for (const auto& e : std::filesystem::recursive_directory_iterator(root)) after += e.is_regular_file();
    CHECK_EQ(after, before);
    core->close();
}

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

TEST_CASE("event-bridge: evidence の guildId=web は web:<room>、payload の sessionId が最優先、壊れた payload は session 無し") {
    w3n::TempDir dir;
    w3n::TestConfig cfg(dir, false);
    auto core = core::create_core(dir.file("db.kuzu"), dir.file("events.jsonl"));
    StubEngine stub;
    auto bridge = create_event_bridge(*core, stub.bridge(), {ws, 60'000, std::nullopt, {}});
    tick();
    const auto gap_id = make_gap(*core, "web 議題");
    core->raw().run("UPDATE design_gaps SET evidence_json = ? WHERE id = ?", {R"({"guildId":"web","channelId":"room-1"})", gap_id});
    bridge->poll_once();
    CHECK_EQ(core->raw().get("SELECT scene FROM sessions WHERE title = ?", {"discussion-of-gap:" + gap_id})->str("scene"), std::string("web:room-1"));

    tick();
    const double t = static_cast<double>(time::now_ms());
    core->raw().run("INSERT INTO events (id, event_type, payload_json, created_at) VALUES ('c1', 'Custom', ?, ?)",
                    {R"({"sessionId":"s-explicit","id":"x"})", t});
    core->raw().run("INSERT INTO events (id, event_type, payload_json, created_at) VALUES ('c2', 'Custom2', '[1,2]', ?)", {t + 1});
    { std::lock_guard l(stub.m); stub.fired.clear(); }
    // 前回の poll で作った gap session の SessionCreated も今回読む。
    CHECK(bridge->poll_once() >= 2);
    {
        std::lock_guard l(stub.m);
        int seen = 0;
        for (const auto& f : stub.fired) {
            if (f.kind == "Custom") {
                CHECK_EQ(f.sessionId.value_or(""), std::string("s-explicit"));
                seen += 1;
            }
            if (f.kind == "Custom2") {
                CHECK(!f.sessionId);
                CHECK(!f.payload);
                seen += 1;
            }
        }
        CHECK_EQ(seen, 2);
    }
    // 終結イベント: resetSession。personas/rules の無いスタブでは記録できない旨を既定の warn に流す (落ちない)。
    core->raw().run("INSERT INTO events (id, event_type, payload_json, created_at) VALUES ('c3', 'HypothesisRejected', ?, ?)",
                    {R"({"id":"h-x","sessionId":"s-close"})", t + 2});
    CHECK_EQ(bridge->poll_once(), 1);
    CHECK_EQ(stub.resets.back(), std::string("s-close"));
    bridge->stop();
    core->close();
}

TEST_CASE("境界: 1 回の poll は kMaxEventsPerPoll まで (同じ created_at の末尾は次回)・巨大 payload は読まない・fire の例外は on_error") {
    w3n::TempDir dir;
    w3n::TestConfig cfg(dir, false);
    auto core = core::create_core(dir.file("db.kuzu"), dir.file("events.jsonl"));
    StubEngine stub;
    std::vector<std::string> errors;
    EventBridgeOptions bo;
    bo.workspaceId = ws;
    bo.pollMs = 60'000;
    bo.onError = [&](const std::string& e, const DiscatierEventRow*) { errors.push_back(e); };
    auto bridge = create_event_bridge(*core, stub.bridge(), bo);
    const double base = bridge->last_seen_at() + 1000;
    // events 表へ直接: kMax ちょうど + 末尾 2 件が同じ created_at。
    core->raw().transaction([&] {
        for (std::size_t i = 0; i < kMaxEventsPerPoll; ++i)
            core->raw().run("INSERT INTO events (id, event_type, payload_json, created_at) VALUES (?, 'Tick', '{}', ?)",
                            {"e" + std::to_string(100000 + i), base + static_cast<double>(i)});
        core->raw().run("INSERT INTO events (id, event_type, payload_json, created_at) VALUES ('zz1', 'Tick', '{}', ?)",
                        {base + static_cast<double>(kMaxEventsPerPoll - 1)});
    });
    // 末尾 (kMax-1 番目の created_at) の 2 件を次回に回すので kMax-1 件。
    CHECK_EQ(bridge->poll_once(), static_cast<int>(kMaxEventsPerPoll - 1));
    CHECK_EQ(bridge->poll_once(), 2);
    CHECK_EQ(bridge->poll_once(), 0);

    // 巨大 payload (上限 + 1) は object として読まない (session 解決なし・落ちない)。
    std::string big = R"({"sessionId":"s","x":")" + std::string(kEventPayloadLimits.max_bytes, 'a') + "\"}";
    core->raw().run("INSERT INTO events (id, event_type, payload_json, created_at) VALUES ('big', 'Tick', ?, ?)", {big, base + 1e7});
    { std::lock_guard l(stub.m); stub.fired.clear(); }
    CHECK_EQ(bridge->poll_once(), 1);
    { std::lock_guard l(stub.m); CHECK(!stub.fired.at(0).payload); }
    CHECK(!parse_event_payload("{broken").has_value());
    CHECK(!parse_event_payload("[1]").has_value());
    CHECK(parse_event_payload("{}").has_value());

    // fire の例外は on_error に流し、その行の lastSeen は進めない (後続の行は処理する)。
    auto throwing = stub.bridge();
    throwing.fireEvent = [](const pe::engine::EngineEvent& e) {
        if (e.kind == "Boom") throw std::runtime_error("boom");
    };
    auto bridge2 = create_event_bridge(*core, throwing, bo);
    core->raw().run("INSERT INTO events (id, event_type, payload_json, created_at) VALUES ('b1', 'Boom', '{}', ?)", {base + 2e7});
    core->raw().run("INSERT INTO events (id, event_type, payload_json, created_at) VALUES ('b2', 'Ok', '{}', ?)", {base + 2e7 + 1});
    CHECK_EQ(bridge2->poll_once(), 1);
    CHECK(!errors.empty() && errors.back() == "boom");
    CHECK_EQ(bridge2->last_seen_at(), base + 2e7 + 1);
    bridge2->stop();
    bridge->stop();
    core->close();
}

TEST_CASE("event-bridge: start で global_scheduler から poll される・stop で止まる・二重 start しない") {
    w3n::TempDir dir;
    w3n::TestConfig cfg(dir, false);
    auto core = core::create_core(dir.file("db.kuzu"), dir.file("events.jsonl"));
    StubEngine stub;
    auto bridge = create_event_bridge(*core, stub.bridge(), {ws, 20, std::nullopt, {}});
    bridge->start();
    bridge->start();
    tick();
    make_gap(*core, "scheduled");
    for (int i = 0; i < 300 && stub.count("DesignGapDetected") == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK_EQ(stub.count("DesignGapDetected"), std::size_t{1});
    bridge->stop();
    const auto n = stub.count("DesignGapDetected");
    make_gap(*core, "after stop");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK_EQ(stub.count("DesignGapDetected"), n);
    core->close();
}

DI_TEST_MAIN()
