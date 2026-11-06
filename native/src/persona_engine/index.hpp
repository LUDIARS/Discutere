#pragma once
// persona-engine — public API (src/persona-engine/index.ts, Phase 0)。
// 内部実装 (db/, engine/, llm/, seeds/) は consumer から直接 include しない (このヘッダから使う)。
//
// TS の再 export のうち AnthropicSdkClient (API キー / OAuth で api.anthropic.com を直接呼ぶ経路) は移さない
// (w1-B の port-notes 参照)。LLM は claude -p / local / mock / worker-pool のみ。
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/sqlite.hpp"
#include "persona_engine/context_provider.hpp"
#include "persona_engine/db/migrations.hpp"
#include "persona_engine/db/personas_repo.hpp"
#include "persona_engine/db/rules_repo.hpp"
#include "persona_engine/db/session_fires_repo.hpp"
#include "persona_engine/engine/engine.hpp"
#include "persona_engine/learning/learner.hpp"
#include "persona_engine/learning/metrics.hpp"
#include "persona_engine/llm/claude_cli.hpp"
#include "persona_engine/llm/client.hpp"
#include "persona_engine/llm/local_openai.hpp"
#include "persona_engine/llm/mock.hpp"
#include "persona_engine/seeds/personas.hpp"
#include "persona_engine/seeds/rules.hpp"
#include "persona_engine/types.hpp"
#include "persona_engine/worker_pool/client.hpp"

namespace di::persona_engine {

struct PersonaEngineOptions {
    sql::DatabasePtr db;
    LLMClientPtr llm;
    std::shared_ptr<DiscussionContextProvider> contextProvider;
    /** 省略時は console_logger()。 */
    std::shared_ptr<Logger> logger;
    std::string workspaceId;
    /** false で seed を入れない (テスト用) */
    bool seedDefaults = true;
    std::optional<std::vector<PersonaSeed>> personaSeeds;
    std::optional<std::vector<RuleSeed>> ruleSeeds;
    std::optional<long long> tickMs;
    std::optional<bool> enableLlm;
    std::function<bool()> rulesDisabled;
    std::function<std::optional<std::string>(const engine::EngineEvent&)> resolveSessionId;
    /** Safety: 同一 session 内総発火上限 (turn budget) */
    std::optional<std::int64_t> maxFiresPerSession;
    /** Safety: 同一 session 内同一 rule 発火上限 (無限ループ防止) */
    std::optional<std::int64_t> maxFiresPerRulePerSession;
};

/** EngineHandle + personas / rules の repo (TS の Object.assign(engine, {personas, rules}))。 */
class PersonaEngineHandle : public engine::Engine {
public:
    PersonaEngineHandle(engine::EngineDeps deps, std::shared_ptr<db::PersonasRepo> personas, std::shared_ptr<db::RulesRepo> rules)
        : engine::Engine(std::move(deps)), personas_(std::move(personas)), rules_(std::move(rules)) {}
    db::PersonasRepo& personas() { return *personas_; }
    db::RulesRepo& rules() { return *rules_; }
    std::shared_ptr<db::PersonasRepo> personas_ptr() const { return personas_; }
    std::shared_ptr<db::RulesRepo> rules_ptr() const { return rules_; }

private:
    std::shared_ptr<db::PersonasRepo> personas_;
    std::shared_ptr<db::RulesRepo> rules_;
};

/** migration → seed → engine 作成。per-session fire counter は同じ db に永続化する (restart-recovery)。 */
std::shared_ptr<PersonaEngineHandle> create_persona_engine(const PersonaEngineOptions& opts);

/** "[persona-engine] <msg> <meta JSON>" を di::log に書く既定のロガー (debug は捨てる)。 */
std::shared_ptr<Logger> console_logger();

} // namespace di::persona_engine
