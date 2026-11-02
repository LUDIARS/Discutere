// w3-M: facilitator.ts / consensus-scorer.ts の移植の検証。
// tests/persona-engine/facilitator.test.ts の観点 (evaluate・tickOnce の拡張/止揚収束・収束で session を閉じる) +
// converge_now・onConverged・調整指示の注入・人間意見の下駄・定期実行・合意スコアラー (採点・👍・境界)。
#include "check.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <thread>

#include "base/crypto.hpp"
#include "base/strings.hpp"
#include "base/time.hpp"
#include "core/index.hpp"
#include "core/noise/exclusions.hpp"
#include "core/test_util.hpp"
#include "discord_hook/facilitator_directives.hpp"
#include "discord_hook/reactions.hpp"
#include "persona_engine/facilitator/index.hpp"
#include "persona_engine/facilitator/js_value.hpp"
#include "persona_engine/index.hpp"
#include "persona_engine/llm/mock.hpp"
#include "persona_engine/scoring/consensus_scorer.hpp"

using namespace di;
using namespace di::persona_engine;
namespace pf = di::persona_engine::facilitator;
namespace ps = di::persona_engine::scoring;

namespace {

const std::string WS = "knowledge";

/** persona-engine の postUtterance を core の utterance として記録する最小の provider。 */
class CoreProvider : public DiscussionContextProvider {
public:
    explicit CoreProvider(core::Core& core) : core_(core) {}
    std::vector<ContextHypothesis> list_active_hypotheses(const std::string&, int) override { return {}; }
    std::vector<ContextGap> list_recent_gaps(const std::string&, int) override { return {}; }
    std::vector<ContextUtterance> list_recent_utterances(const std::string&, const std::string&, int) override { return {}; }
    std::string propose_hypothesis(const ProposeHypothesisInput&) override { return ""; }
    std::string post_utterance(const PostUtteranceInput& input) override {
        core::repositories::UtteranceInput u;
        u.workspaceId = input.workspaceId;
        u.sessionId = input.sessionId;
        u.speakerId = "persona:" + input.byPersonaId;
        u.rawContent = input.text;
        u.postedAt = static_cast<double>(time::now_ms());
        posts.fetch_add(1);
        return core_.repos.utterance.create(u);
    }
    std::atomic<int> posts{0};

private:
    core::Core& core_;
};

class CaptureLogger : public Logger {
public:
    void debug(const Json&, std::string_view) override {}
    void info(const Json&, std::string_view m) override { add("info", m); }
    void warn(const Json& meta, std::string_view m) override { add("warn", std::string(m) + " " + meta.dump()); }
    void error(const Json&, std::string_view m) override { add("error", m); }
    bool has(std::string_view needle) {
        std::lock_guard l(mu);
        for (const auto& s : lines)
            if (str::contains(s, needle)) return true;
        return false;
    }

private:
    void add(const char* level, std::string_view m) {
        std::lock_guard l(mu);
        lines.push_back(std::string(level) + ": " + std::string(m));
    }
    std::mutex mu;
    std::vector<std::string> lines;
};

struct World {
    test::TempDir dir;
    std::shared_ptr<core::Core> core;
    sql::DatabasePtr pe;
    std::shared_ptr<db::PersonasRepo> personas;
    std::shared_ptr<CoreProvider> provider;
    std::shared_ptr<CaptureLogger> logger = std::make_shared<CaptureLogger>();
    std::int64_t old = time::now_ms() - 5'000;
    std::int64_t seq = 0;

    World() {
        core = std::shared_ptr<core::Core>(core::create_core(dir.file("kg.kuzu"), dir.file("events.jsonl")).release());
        pe = sql::Database::open(dir.file("pe.db"));
        db::apply_persona_engine_migrations(*pe);
        personas = std::make_shared<db::PersonasRepo>(pe);
        provider = std::make_shared<CoreProvider>(*core);
    }

    struct Made {
        std::string gapId;
        std::string sessionId;
        std::vector<std::string> utteranceIds;
    };
    Made discussion(const std::string& title, const std::vector<std::string>& speakers, const std::string& scene) {
        core::repositories::DesignGapInput g;
        g.workspaceId = WS;
        g.title = title;
        Made m;
        m.gapId = core->repos.designGap.create(g);
        core::repositories::SessionInput s;
        s.workspaceId = WS;
        s.title = "discussion-of-gap:" + m.gapId;
        s.startedAt = static_cast<double>(old);
        s.scene = scene;
        m.sessionId = core->repos.session.create(s);
        for (std::size_t i = 0; i < speakers.size(); ++i) m.utteranceIds.push_back(say(m.sessionId, speakers[i], "発言" + std::to_string(i)));
        return m;
    }
    std::string say(const std::string& session, const std::string& speaker, const std::string& text) {
        core::repositories::UtteranceInput u;
        u.workspaceId = WS;
        u.sessionId = session;
        u.speakerId = speaker;
        u.rawContent = text;
        u.postedAt = static_cast<double>(old + seq++);  // 同時刻の並びに頼らない
        return core->repos.utterance.create(u);
    }
    std::string gap_status(const std::string& gap) {
        auto r = core->raw().get("SELECT status FROM design_gaps WHERE id = ?", {gap});
        return r ? r->str("status") : "";
    }
    pf::FacilitatorDeps deps(LLMClientPtr llm) {
        pf::FacilitatorDeps d;
        d.core = core;
        d.llm = std::move(llm);
        d.contextProvider = provider;
        d.personas = personas;
        d.workspaceId = WS;
        d.logger = logger;
        d.options.idleGapMs = 1000;
        d.options.maxPersonas = 50;
        d.options.aufhebungTarget = 1;
        return d;
    }
};

using Variant = std::variant<std::string, LLMResult>;

/** facilitator.test.ts の mock LLM (prompt 種別を本文で判別)。 */
LLMClientPtr scripted_llm(std::vector<std::string>* prompts = nullptr, std::mutex* mu = nullptr) {
    MockResponder r = [prompts, mu](const LLMInvokeArgs& args) -> Variant {
        if (prompts) {
            std::lock_guard l(*mu);
            prompts->push_back(args.prompt);
        }
        const auto& u = args.prompt;
        if (str::contains(u, "ストック済みの止揚")) {
            const bool found = str::contains(u, "収束テスト");
            return found ? std::string(R"({ "found": true, "summary": "両論を統合した一段上の結論X" })")
                         : std::string(R"({ "found": false, "summary": "" })");
        }
        if (str::contains(u, "到達した止揚")) return std::string(R"({ "summary": "止揚Xを軸にした結論。" })");
        return std::string(
            R"({ "name": "コスト懐疑", "display_name": "費田 渋", "description": "費用対効果を問う", "traits": ["慎重"], "speech_style": "淡々", "opening": "本当にそれは時間に見合う?" })");
    };
    std::vector<MockResponder> many(64, r);
    return std::make_shared<MockLLMClient>(many);
}

} // namespace

TEST_CASE("evaluate: 新発言で active + 再 arm / idle + armed で intervene / 未達・disarm は wait") {
    auto r = pf::evaluate({5, 3, 0, 9999}, {4, false}, 1000);
    CHECK_EQ(r.mode, std::string("active"));
    CHECK(r.state == (pf::SessionState{5, true}));
    r = pf::evaluate({5, 3, 0, 5000}, {5, true}, 1000);
    CHECK_EQ(r.mode, std::string("intervene"));
    CHECK(r.state == (pf::SessionState{5, false}));
    CHECK_EQ(pf::evaluate({5, 3, 0, 5000}, {5, false}, 1000).mode, std::string("wait"));
    CHECK_EQ(pf::evaluate({5, 3, 4900, 5000}, {5, true}, 1000).mode, std::string("wait"));
    // 境界: idle がちょうど idleGapMs なら介入する (>=)、1 ms 足りなければ待つ
    CHECK_EQ(pf::evaluate({5, 3, 4000, 5000}, {5, true}, 1000).mode, std::string("intervene"));
    CHECK_EQ(pf::evaluate({5, 3, 4001, 5000}, {5, true}, 1000).mode, std::string("wait"));
}

TEST_CASE("tick_once: 止揚が無い議論は新 persona で広げ、止揚到達の議論は収束して gap closed・session ended") {
    World w;
    const auto expand_case = w.discussion("拡張テスト", {"persona:advocate"}, "discord:1/10");
    const auto converge_case = w.discussion("収束テスト", {"persona:advocate"}, "discord:1/20");
    std::vector<pf::FacilitatorConvergedEvent> events;
    auto d = w.deps(scripted_llm());
    d.onConverged = [&](const pf::FacilitatorConvergedEvent& e) { events.push_back(e); };
    auto fac = pf::create_facilitator(d);
    // 進行役 persona は構築時に登録される
    CHECK(w.personas->get("facilitator").has_value());
    CHECK_EQ(w.personas->get("facilitator")->display_name, std::string("司会 結"));

    fac->tick_once();

    const auto expanded = w.core->raw().all("SELECT raw_content FROM utterances WHERE session_id = ? AND speaker_id LIKE 'persona:dyn-%'",
                                            {expand_case.sessionId});
    CHECK_EQ(expanded.size(), std::size_t(1));
    CHECK_EQ(expanded[0].str("raw_content"), std::string("本当にそれは時間に見合う?"));
    // 動的 persona が登録される (name/display_name/traits は LLM の JSON から)
    const auto dyn = w.core->raw().get("SELECT speaker_id FROM utterances WHERE session_id = ? AND speaker_id LIKE 'persona:dyn-%'",
                                       {expand_case.sessionId});
    const auto pid = dyn->str("speaker_id").substr(8);
    CHECK_EQ(str::js_length(pid), std::size_t(12));  // "dyn-" + 8
    CHECK_EQ(w.personas->get(pid)->display_name, std::string("費田 渋"));
    CHECK_EQ(w.personas->get(pid)->traits, std::string(R"(["慎重"])"));

    const auto stock = w.core->raw().all("SELECT summary FROM aufhebung_stock WHERE gap_id = ?", {converge_case.gapId});
    CHECK_EQ(stock.size(), std::size_t(1));
    CHECK(str::contains(stock[0].str("summary"), "統合"));
    const auto conv = w.core->raw().get("SELECT raw_content FROM utterances WHERE session_id = ? AND speaker_id = 'persona:facilitator'",
                                        {converge_case.sessionId});
    CHECK(conv && str::contains(conv->str("raw_content"), "【収束】"));
    CHECK_EQ(conv->str("raw_content"), std::string("【収束】\n止揚Xを軸にした結論。"));
    CHECK_EQ(w.gap_status(converge_case.gapId), std::string("closed"));
    CHECK(!w.core->raw().get("SELECT ended_at FROM sessions WHERE id = ?", {converge_case.sessionId})->is_null("ended_at"));
    CHECK(w.core->raw().get("SELECT ended_at FROM sessions WHERE id = ?", {expand_case.sessionId})->is_null("ended_at"));

    CHECK_EQ(events.size(), std::size_t(1));
    CHECK_EQ(events[0].gapId, converge_case.gapId);
    CHECK_EQ(events[0].scene.value_or(""), std::string("discord:1/20"));
    CHECK_EQ(events[0].title, std::string("収束テスト"));
    CHECK_EQ(events[0].summary, std::string("止揚Xを軸にした結論。"));

    // 2 回目の tick: 収束済み (closed) は対象外、拡張済みは新発言で active になるだけ (再介入しない)
    const int before = w.provider->posts.load();
    fac->tick_once();
    CHECK_EQ(w.provider->posts.load(), before);
}

TEST_CASE("tick_once: headless (scene=gap:) も駆動し、scene の無い/他の scene・別 workspace・dismissed は対象外") {
    World w;
    const auto headless = w.discussion("拡張テスト headless", {"persona:a"}, "gap:x");
    const auto web = w.discussion("拡張テスト web", {"persona:a"}, "web:room");
    const auto dismissed = w.discussion("拡張テスト dismissed", {"persona:a"}, "discord:1/30");
    w.core->raw().run("UPDATE design_gaps SET status = 'dismissed' WHERE id = ?", {dismissed.gapId});
    auto fac = pf::create_facilitator(w.deps(scripted_llm()));
    fac->tick_once();
    auto count = [&](const std::string& s) {
        return w.core->raw().get("SELECT COUNT(*) AS n FROM utterances WHERE session_id = ? AND speaker_id LIKE 'persona:dyn-%'", {s})->i64("n");
    };
    CHECK_EQ(count(headless.sessionId), 1);
    CHECK_EQ(count(web.sessionId), 0);
    CHECK_EQ(count(dismissed.sessionId), 0);
}

TEST_CASE("intervene: maxPersonas 超過で収束、aufhebung-only ポリシーでは persona 数で収束しない (ライブ tuning)") {
    World w;
    const auto many = w.discussion("拡張テスト 多人数", {"persona:a", "persona:b", "persona:c"}, "discord:1/40");
    runtime_settings::FacilitatorTuning t{30'000, 1000, 2, 9, "aufhebung-only"};
    auto d = w.deps(scripted_llm());
    d.tuning = [&] { return t; };
    auto fac = pf::create_facilitator(d);
    fac->tick_once();
    CHECK_EQ(w.gap_status(many.gapId), std::string(""));  // 拡張しただけ

    World w2;
    const auto many2 = w2.discussion("拡張テスト 多人数", {"persona:a", "persona:b", "persona:c"}, "discord:1/40");
    t.convergePolicy = "default";
    auto d2 = w2.deps(scripted_llm());
    d2.tuning = [&] { return t; };
    pf::create_facilitator(d2)->tick_once();
    CHECK_EQ(w2.gap_status(many2.gapId), std::string("closed"));  // 3 > maxPersonas(2)
}

TEST_CASE("prompt: 調整指示は topic に注入され、除外発話は載らず、人間の意見は下駄で上位に来る") {
    World w;
    const auto c = w.discussion("収束テスト 指示", {"persona:a", "user-123"}, "discord:1/50");
    discord_hook::add_facilitator_directive(w.core->raw(), {WS, c.gapId, "もっと簡単な言葉で", std::string("author-9")});
    const auto noisy = w.say(c.sessionId, "persona:b", "ノイズ発言ZZZ");
    core::noise::exclude_utterance(w.core->raw(), noisy, std::string("noise"));
    std::vector<std::string> prompts;
    std::mutex mu;
    auto fac = pf::create_facilitator(w.deps(scripted_llm(&prompts, &mu)));
    fac->tick_once();
    CHECK_EQ(w.gap_status(c.gapId), std::string("closed"));
    bool directive = false, author = false, noise = false, human_top = false;
    for (const auto& p : prompts) {
        directive = directive || str::contains(p, "もっと簡単な言葉で");
        author = author || str::contains(p, "author-9");
        noise = noise || str::contains(p, "ZZZ");
        human_top = human_top || str::contains(p, "人間「発言1」(+5 / 人間)");
    }
    CHECK(directive);
    CHECK(!author);  // author_id は prompt に出さない
    CHECK(!noise);
    CHECK(human_top);
}

TEST_CASE("converge_now: gap 無し・既に closed・LLM 失敗は ok=false、成功は ok=true、フック例外は握る") {
    World w;
    const auto c = w.discussion("任意の議題", {"persona:a"}, "discord:1/60");
    auto failing = std::make_shared<MockLLMClient>(std::vector<MockResponder>{
        [](const LLMInvokeArgs&) -> Variant { return LLMResult::failure("backend down"); }});
    auto fac = pf::create_facilitator(w.deps(failing));
    auto r = fac->converge_now(c.sessionId, "no-such-gap");
    CHECK(!r.ok);
    CHECK_EQ(r.error.value_or(""), std::string("gap not found"));
    r = fac->converge_now(c.sessionId, c.gapId);
    CHECK(!r.ok);
    CHECK_EQ(r.error.value_or(""), std::string("まとめ生成に失敗しました (LLM 応答なし)。再試行してください"));
    CHECK(w.logger->has("facilitator converge llm failed"));

    // JSON でない応答はそのまま本文にする
    auto plain = std::make_shared<MockLLMClient>(std::vector<MockResponder>{[](const LLMInvokeArgs&) -> Variant { return std::string("素のまとめ"); }});
    auto d = w.deps(plain);
    d.onConverged = [](const pf::FacilitatorConvergedEvent&) { throw std::runtime_error("hook boom"); };
    auto fac2 = pf::create_facilitator(d);
    r = fac2->converge_now(c.sessionId, c.gapId);
    CHECK(r.ok);
    CHECK(w.logger->has("facilitator onConverged hook failed"));
    CHECK_EQ(w.core->raw().get("SELECT raw_content FROM utterances WHERE session_id = ? AND speaker_id = 'persona:facilitator'", {c.sessionId})->str("raw_content"),
             std::string("【収束】\n素のまとめ"));
    r = fac2->converge_now(c.sessionId, c.gapId);
    CHECK(!r.ok);
    CHECK_EQ(r.error.value_or(""), std::string("already closed"));
}

TEST_CASE("expand: 壊れた JSON・opening 無し・LLM 失敗では persona を足さない (巨大応答も上限で拒否)") {
    for (const std::string& text : {std::string("not json"), std::string(R"({"name":"x"})"), std::string(R"({"opening": 5})"),
                                    "{\"opening\":\"" + std::string(2u << 20, 'a') + "\"}"}) {
        World w;
        const auto c = w.discussion("拡張テスト 壊れ", {"persona:a"}, "discord:1/70");
        auto llm = std::make_shared<MockLLMClient>(std::vector<MockResponder>{
            [](const LLMInvokeArgs&) -> Variant { return std::string(R"({"found": false})"); },
            [text](const LLMInvokeArgs&) -> Variant { return text; }});
        pf::create_facilitator(w.deps(llm))->tick_once();
        CHECK_EQ(w.provider->posts.load(), 0);
        CHECK_EQ(w.gap_status(c.gapId), std::string(""));
    }
}

TEST_CASE("start/stop: 定期 tick が background で走り、stop 後は走らない・二重 start は無視") {
    World w;
    const auto c = w.discussion("拡張テスト 定期", {"persona:a"}, "discord:1/80");
    auto d = w.deps(scripted_llm());
    d.options.tickMs = 10;
    auto fac = pf::create_facilitator(d);
    fac->start();
    fac->start();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (w.provider->posts.load() == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(w.provider->posts.load() >= 1);
    fac->stop();
    const int after = w.provider->posts.load();
    w.say(c.sessionId, "persona:z", "再開");  // 新発言 → 次 tick で再 arm されうるが stop 済みなので走らない
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK_EQ(w.provider->posts.load(), after);
}

// ─── JS の値変換 (golden) ────────────────────────────────────────

TEST_CASE("js_value: Number(x) / String(x) が Node と一致 (golden)") {
    for (const auto& c : test::load_fixture("w3m/js_values.json")) {
        const Json& v = c["v"];
        const double n = pf::js_number_of(&v);
        if (c["num"] == "NaN") {
            CHECK(std::isnan(n));
        } else {
            CHECK_EQ(n, c["num"].get<double>());
        }
        CHECK_EQ(pf::js_string(v), c["str"].get<std::string>());
    }
    CHECK(std::isnan(pf::js_number_of(nullptr)));  // undefined
}

// ─── consensus scorer ───────────────────────────────────────────

TEST_CASE("parse_verdicts: 配列の切り出し・i は整数のみ・score は Number||0・壊れた入力と上限") {
    auto v = ps::parse_verdicts(R"(説明 [{"i":0,"agree":true,"score":4,"reasoning":"r"},{"i":"1","agree":"yes","score":"x"},{"i":1.5},{"x":1},5,[1]] 後書き)");
    CHECK(v.has_value());
    CHECK_EQ(v->size(), std::size_t(2));
    CHECK_EQ((*v)[0].i, 0.0);
    CHECK((*v)[0].agree);
    CHECK_EQ((*v)[0].score, 4.0);
    CHECK_EQ((*v)[0].reasoning.value_or(""), std::string("r"));
    CHECK_EQ((*v)[1].i, 1.0);
    CHECK(!(*v)[1].agree);
    CHECK_EQ((*v)[1].score, 0.0);
    CHECK(!ps::parse_verdicts("no array").has_value());
    CHECK(!ps::parse_verdicts("] [").has_value());
    CHECK(!ps::parse_verdicts("[{broken").has_value());
    CHECK(!ps::parse_verdicts(R"([1, 2] {"a": [}])").has_value());
    // 上限: ちょうど (1 MiB) は読める、+1 は拒否、深い入れ子は拒否
    const auto limit = ps::verdict_json_limits().max_bytes;
    std::string exact = "[\"" + std::string(limit - 4, 'a') + "\"]";
    CHECK_EQ(exact.size(), limit);
    CHECK(ps::parse_verdicts(exact).has_value());
    std::string over = "[\"" + std::string(limit - 3, 'a') + "\"]";
    CHECK(!ps::parse_verdicts(over).has_value());
    CHECK(!ps::parse_verdicts(str::repeat("[", 10'000) + str::repeat("]", 10'000)).has_value());
}

TEST_CASE("score_once: 未採点の意見 (persona/人間) を最大 8 件採点し、同意は opinion_scores に加点・👍 を付ける") {
    World w;
    std::vector<std::string> speakers{"persona:facilitator", "ext:yt-1", "persona:a", "user-1"};
    for (int i = 0; i < 8; ++i) speakers.push_back("persona:p" + std::to_string(i));
    const auto c = w.discussion("合意テスト", speakers, "discord:1/90");
    w.say(c.sessionId, "persona:a", "【収束】\nまとめ");
    discord_hook::ensure_reaction_tables(w.core->raw());
    // utterance 2 (persona:a) は Discord に投稿済み
    discord_hook::record_posted_message(w.core->raw(), {"m-1", c.utteranceIds[2], "utterance", std::string("ch-1"), std::nullopt});
    std::vector<std::string> prompts;
    MockResponder respond = [&](const LLMInvokeArgs& a) -> Variant {
        prompts.push_back(a.prompt);
        CHECK_EQ(a.maxTokens.value_or(0), 700);
        return std::string(R"([{"i":0,"agree":true,"score":4.5},{"i":1,"agree":false,"score":3},{"i":2,"agree":true,"score":9},{"i":99,"agree":true,"score":5},{"i":-1,"agree":true,"score":5}])");
    };
    auto llm = std::make_shared<MockLLMClient>(std::vector<MockResponder>{respond, respond});
    std::vector<ps::ConsensusReactInput> reacts;
    ps::ConsensusScorerDeps deps;
    deps.core = w.core;
    deps.llm = llm;
    deps.intervalMs = 1000;
    deps.logger = w.logger;
    deps.react = [&](const ps::ConsensusReactInput& r) {
        reacts.push_back(r);
        throw std::runtime_error("discord down");  // 失敗しても採点は続ける
    };
    auto scorer = ps::create_consensus_scorer(deps);
    scorer->score_once();

    CHECK_EQ(prompts.size(), std::size_t(1));
    // facilitator / ext: / 【収束】 は除外、posted_at 昇順で最大 8 件 → 0:persona:a 1:user-1 2..7:p0..p5 (【収束】は LIMIT の外)
    CHECK(str::contains(prompts[0], "0: 発言2"));
    CHECK(str::contains(prompts[0], "1: 発言3"));
    CHECK(!str::contains(prompts[0], "発言0"));
    CHECK(!str::contains(prompts[0], "発言1\n"));
    const auto scores = w.core->raw().all("SELECT utterance_id, agree, score FROM consensus_scores ORDER BY score DESC");
    CHECK_EQ(scores.size(), std::size_t(3));
    CHECK_EQ(w.core->raw().get("SELECT score FROM consensus_scores WHERE utterance_id = ?", {c.utteranceIds[2]})->i64("score"), 5);  // round(4.5)
    CHECK_EQ(w.core->raw().get("SELECT agree FROM consensus_scores WHERE utterance_id = ?", {c.utteranceIds[3]})->i64("agree"), 0);
    CHECK_EQ(discord_hook::get_opinion_score(w.core->raw(), c.utteranceIds[2]), 5);
    CHECK_EQ(discord_hook::get_opinion_score(w.core->raw(), c.utteranceIds[3]), 0);  // 不同意は加点しない
    CHECK_EQ(discord_hook::get_opinion_score(w.core->raw(), c.utteranceIds[4]), 5);  // 9 → 5 にクランプ
    CHECK_EQ(reacts.size(), std::size_t(1));  // mapping があるのは 1 件だけ
    CHECK_EQ(reacts[0].messageId, std::string("m-1"));
    CHECK_EQ(reacts[0].channelId, std::string("ch-1"));
    CHECK_EQ(reacts[0].emoji, std::string("👍"));
    CHECK(w.logger->has("consensus scorer react failed"));

    // 2 回目は採点済みを除いた残り
    scorer->score_once();
    CHECK_EQ(prompts.size(), std::size_t(2));
    CHECK(!str::contains(prompts[1], "発言2"));
}

TEST_CASE("score_once: LLM 失敗・壊れた応答は何も記録しない / closed の議論は対象外") {
    World w;
    const auto c = w.discussion("合意テスト 失敗", {"persona:a"}, "gap:z");
    const auto closed = w.discussion("合意テスト closed", {"persona:a"}, "gap:y");
    w.core->raw().run("UPDATE design_gaps SET status = 'closed' WHERE id = ?", {closed.gapId});
    int calls = 0;
    auto llm = std::make_shared<MockLLMClient>(std::vector<MockResponder>{
        [&](const LLMInvokeArgs&) -> Variant { ++calls; return LLMResult::failure("x"); },
        [&](const LLMInvokeArgs&) -> Variant { ++calls; return std::string("no verdict"); }});
    ps::ConsensusScorerDeps deps;
    deps.core = w.core;
    deps.llm = llm;
    deps.logger = w.logger;
    auto scorer = ps::create_consensus_scorer(deps);
    scorer->score_once();
    scorer->score_once();
    CHECK_EQ(calls, 2);
    CHECK_EQ(w.core->raw().get("SELECT COUNT(*) AS n FROM consensus_scores")->i64("n"), 0);
    CHECK(w.logger->has("consensus scorer llm failed"));
    CHECK(w.logger->has("consensus scorer parse failed"));
}

DI_TEST_MAIN()
