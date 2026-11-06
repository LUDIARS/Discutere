#pragma once
// w3m_facilitator_tests.cpp を Praesidium の静的解析 (1 単位 60 秒) に収まるよう分けた各パートが共有する準備。
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

namespace w3m_facilitator_support {

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

} // namespace w3m_facilitator_support
