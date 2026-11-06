#pragma once
// w3m_admin_routes_tests.cpp を Praesidium の静的解析 (1 単位 60 秒) に収まるよう分けた各パートが共有する準備。
// w3-M: api/admin-routes.ts・consensus-persona-routes.ts・gap-creation-routes.ts の移植の検証 (TS テスト無し)。
// 認可 (401/403)・late-binding の 503・各ルートの効果と応答の形・本文の境界 (上限ちょうど/+1・壊れた JSON・巨大)。
#include "check.hpp"

#include <filesystem>

#include "api/admin_routes.hpp"
#include "api/consensus_persona_routes.hpp"
#include "api/gap_creation_routes.hpp"
#include "base/env.hpp"
#include "base/strings.hpp"
#include "base/time.hpp"
#include "config.hpp"
#include "core/index.hpp"
#include "core/test_util.hpp"
#include "discord_hook/reactions.hpp"
#include "middleware/auth.hpp"
#include "persona_engine/facilitator/facilitator.hpp"
#include "persona_engine/index.hpp"
#include "persona_engine/llm/mock.hpp"
#include "persona_engine/scoring/consensus_scorer.hpp"
#include "w1f_support.hpp"

using namespace di;
using di::test::w1f::body_json;
using di::test::w1f::make_request;

namespace w3m_admin_routes_support {

const std::string WS = "ws-test";

class NoopProvider : public persona_engine::DiscussionContextProvider {
public:
    explicit NoopProvider(core::Core* core = nullptr) : core_(core) {}
    std::vector<persona_engine::ContextHypothesis> list_active_hypotheses(const std::string&, int) override { return {}; }
    std::vector<persona_engine::ContextGap> list_recent_gaps(const std::string&, int) override { return {}; }
    std::vector<persona_engine::ContextUtterance> list_recent_utterances(const std::string&, const std::string&, int) override {
        return {};
    }
    std::string propose_hypothesis(const persona_engine::ProposeHypothesisInput&) override { return ""; }
    std::string post_utterance(const persona_engine::PostUtteranceInput& in) override {
        if (!core_) return "";
        core::repositories::UtteranceInput u;
        u.workspaceId = in.workspaceId;
        u.sessionId = in.sessionId;
        u.speakerId = "persona:" + in.byPersonaId;
        u.rawContent = in.text;
        u.postedAt = static_cast<double>(time::now_ms());
        return core_->repos.utterance.create(u);
    }

private:
    core::Core* core_;
};

/** active KG / persona-engine DB / workspace を一時ディレクトリへ向けた環境。 */
struct Env {
    test::TempDir dir;
    std::string kg;
    std::string pe;
    Env() {
        kg = dir.file("kg.kuzu");
        pe = dir.file("pe.db");
        env::set_for_test("DISCUTERE_CONFIG", std::string_view(dir.file("no-config.json")));
        env::set_for_test("DISCATIER_KUZU_PATH", std::string_view(kg));
        env::set_for_test("DISCUTERE_PERSONA_ENGINE_DB", std::string_view(pe));
        env::set_for_test("DISCATIER_WORKSPACE", std::string_view(WS));
        config::reset_config_for_test();
        api::set_persona_engine(nullptr);
        api::set_facilitator(nullptr);
        api::set_gateway_sweeper(nullptr);
    }
    ~Env() {
        api::set_persona_engine(nullptr);
        api::set_facilitator(nullptr);
        api::set_gateway_sweeper(nullptr);
        config::reset_config_for_test();
    }
    std::unique_ptr<core::Core> core() { return core::create_core(kg, dir.file("events.jsonl")); }
};

http::Router app() {
    http::Router r;
    r.use(middleware::user_context());
    r.mount("/api", api::admin_routes());
    r.mount("/api", api::consensus_persona_routes());
    r.mount("/api", api::gap_creation_routes());
    return r;
}

http::Response call(const http::Router& r, const std::string& method, const std::string& target, std::string body = "",
                    const Json& headers = Json::object(), const std::string& remote = "127.0.0.1") {
    auto req = make_request(method, target, headers, remote, std::move(body));
    return r.handle(req);
}

struct Gap {
    std::string gapId;
    std::string sessionId;
};
Gap make_gap(core::Core& c, const std::string& title, const std::string& scene, const std::optional<std::string>& status = std::nullopt) {
    core::repositories::DesignGapInput g;
    g.workspaceId = WS;
    g.title = title;
    g.status = status;
    Gap out;
    out.gapId = c.repos.designGap.create(g);
    core::repositories::SessionInput s;
    s.workspaceId = WS;
    s.title = "discussion-of-gap:" + out.gapId;
    s.startedAt = 1000;
    s.scene = scene;
    out.sessionId = c.repos.session.create(s);
    return out;
}

std::shared_ptr<persona_engine::PersonaEngineHandle> make_engine(const std::string& pe_path) {
    persona_engine::PersonaEngineOptions o;
    o.db = sql::Database::open(pe_path);
    o.llm = std::make_shared<persona_engine::MockLLMClient>(std::vector<persona_engine::MockResponder>{});
    o.contextProvider = std::make_shared<NoopProvider>();
    o.workspaceId = WS;
    o.enableLlm = false;
    return persona_engine::create_persona_engine(o);
}

const std::vector<std::pair<std::string, std::string>> kAdminRoutes = {
    {"POST", "/api/admin/rules/enabled"},       {"POST", "/api/admin/session/reset"},    {"POST", "/api/admin/gaps/g1/dismiss"},
    {"POST", "/api/admin/gaps/dismiss-open"},   {"POST", "/api/admin/sessions/s1/close"}, {"POST", "/api/admin/sessions/s1/converge"},
    {"GET", "/api/admin/status"},               {"POST", "/api/admin/seed-sweep"},       {"GET", "/api/admin/learning/topics"},
    {"GET", "/api/admin/metrics/personas"},     {"GET", "/api/admin/metrics/rules"},     {"GET", "/api/admin/rules"},
    {"GET", "/api/admin/rules/r1/logs"},        {"GET", "/api/admin/consensus/g1/personas"}, {"POST", "/api/admin/gaps"},
};

} // namespace w3m_admin_routes_support
