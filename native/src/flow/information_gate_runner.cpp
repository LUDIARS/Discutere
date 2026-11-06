// src/flow/information-gate-runner.ts の移植。
#include "flow/information_gate_runner.hpp"

#include <cmath>

#include "config.hpp"
#include "flow/cost_logger.hpp"
#include "flow/db/connection.hpp"
#include "flow/density_blackbox.hpp"
#include "flow/learning_autocrawl.hpp"

namespace di::flow {

namespace {

/** config の minDensity (string) を厳格な InformationDensity に倒す。 */
InformationDensity resolve_min_density(const std::string& raw) { return is_density(raw) ? raw : "moderate"; }

int to_int(double v) {
    if (!std::isfinite(v)) return 0;
    return static_cast<int>(std::trunc(v));
}

} // namespace

std::optional<InformationGateResult> gate_before_flow(const FlowGateArgs& args) {
    const auto& flow_cfg = config::get_config().flow;
    const auto& gate_cfg = flow_cfg.informationGating;
    const auto& crawl = flow_cfg.autoCrawl;

    if (!gate_cfg.enabled) return std::nullopt;
    if (args.kind != "discussion" && args.kind != "improvement") return std::nullopt;
    if (!args.openCore) return std::nullopt;
    if (!args.llm) return std::nullopt;

    const auto crawl_sources = resolve_auto_crawl_sources(crawl.sources, args.youtubeApiKey);
    if (crawl_sources.empty()) return std::nullopt;

    const auto log = args.log ? args.log : [](const std::string&) {};
    const auto warn = args.warn ? args.warn : [](const std::string&) {};

    InformationGateConfig config;
    config.enabled = true;
    config.minDensity = resolve_min_density(gate_cfg.minDensity);
    config.maxLearnIterations = to_int(gate_cfg.maxLearnIterations);
    config.maxGapsPerIteration = to_int(gate_cfg.maxGapsPerIteration);
    if (!gate_cfg.model.empty()) config.model = gate_cfg.model;

    auto core = args.openCore();
    struct Closer {
        core::Core* c;
        ~Closer() {
            if (c) c->close();
        }
    } closer{core.get()};

    auto llm = with_cost_log(*args.llm, CostLogContext{args.kind, args.sessionId, std::nullopt, std::nullopt, std::nullopt,
                                                        std::nullopt, "information-gate", std::nullopt});
    // 密度判定は blackbox 経由 (卒業ルールがあれば LLM 省略、無ければ LLM 判定を教師に学習)。
    EvaluateFn evaluate_fn = make_blackbox_density_evaluator(make_density_blackbox(db::get_flow_db()),
                                                             args.evaluateFn ? args.evaluateFn : EvaluateFn(evaluate_information_density));

    InformationGateDeps deps;
    deps.core = core.get();
    deps.theme = args.theme;
    deps.tags = args.tags;
    deps.workspaceId = args.workspaceId;
    deps.llm = &llm;
    deps.listExternalVoices = args.listExternalVoices;
    deps.crawlSources = crawl_sources;
    deps.maxItems = to_int(crawl.maxItems);
    deps.youtubeApiKey = args.youtubeApiKey;
    deps.config = config;
    deps.collectAndImportFn = args.collectAndImportFn;
    deps.evaluateFn = evaluate_fn;
    deps.log = log;
    deps.warn = warn;
    return run_information_gate(deps);
}

PaperDebatabilityGate DebatabilityGateOptions::to_paper_gate() const {
    PaperDebatabilityGate g;
    g.llm = llm.get();
    g.minArmableIssues = minArmableIssues;
    return g;
}

std::optional<DebatabilityGateOptions> resolve_debatability_gate(const FlowKind& kind, const std::string& session_id,
                                                                 persona_engine::LLMClient& llm) {
    const auto& cfg = config::get_config().flow.debatability;
    if (!cfg.enabled) return std::nullopt;
    if (kind != "discussion" && kind != "improvement") return std::nullopt;
    DebatabilityGateOptions o;
    o.llm = std::make_shared<CostLoggedClient>(
        llm, CostLogContext{kind, session_id, std::nullopt, std::nullopt, std::nullopt, std::nullopt, "debatability-gate", std::nullopt});
    o.minArmableIssues = to_int(cfg.minArmableIssues);
    return o;
}

} // namespace di::flow
