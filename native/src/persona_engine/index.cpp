#include "persona_engine/index.hpp"

#include <stdexcept>

#include "base/log.hpp"

namespace di::persona_engine {

namespace {

class ConsoleLogger : public Logger {
public:
    void debug(const Json&, std::string_view) override {}
    void info(const Json& meta, std::string_view message) override { log::info(line(meta, message)); }
    void warn(const Json& meta, std::string_view message) override { log::warn(line(meta, message)); }
    void error(const Json& meta, std::string_view message) override { log::error(line(meta, message)); }

private:
    static std::string line(const Json& meta, std::string_view message) {
        return "[persona-engine] " + std::string(message) + " " + json_stringify(meta);
    }
};

} // namespace

std::shared_ptr<Logger> console_logger() { return std::make_shared<ConsoleLogger>(); }

std::shared_ptr<PersonaEngineHandle> create_persona_engine(const PersonaEngineOptions& opts) {
    if (!opts.db) throw std::invalid_argument("create_persona_engine: db required");
    db::apply_persona_engine_migrations(*opts.db);

    auto personas = std::make_shared<db::PersonasRepo>(opts.db);
    auto rules = std::make_shared<db::RulesRepo>(opts.db);
    auto logger = opts.logger ? opts.logger : console_logger();

    if (opts.seedDefaults) {
        personas->bulk_seed(opts.personaSeeds ? *opts.personaSeeds : seeds::DISCUSSION_PERSONA_SEEDS());
        rules->bulk_seed(opts.ruleSeeds ? *opts.ruleSeeds : seeds::DISCUSSION_RULE_SEEDS());
    }

    engine::EngineDeps deps;
    deps.personas = personas;
    deps.rules = rules;
    deps.llm = opts.llm;
    deps.contextProvider = opts.contextProvider;
    deps.logger = logger;
    deps.workspaceId = opts.workspaceId;
    deps.tickMs = opts.tickMs;
    deps.enableLlm = opts.enableLlm;
    deps.rulesDisabled = opts.rulesDisabled;
    deps.resolveSessionId = opts.resolveSessionId;
    deps.maxFiresPerSession = opts.maxFiresPerSession;
    deps.maxFiresPerRulePerSession = opts.maxFiresPerRulePerSession;
    // restart-recovery: per-session fire counter を同一 db に永続化し、再起動越しに safety cap を効かせる。
    deps.sessionFireStore = std::make_shared<db::SqliteSessionFireStore>(opts.db);
    return std::make_shared<PersonaEngineHandle>(std::move(deps), personas, rules);
}

} // namespace di::persona_engine
