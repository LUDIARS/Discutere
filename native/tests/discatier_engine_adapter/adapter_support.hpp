#pragma once
// adapter_tests.cpp を Praesidium の静的解析 (1 単位 60 秒) に収まるよう分けた各パートが共有する準備。
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

namespace adapter_support {

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

} // namespace adapter_support
