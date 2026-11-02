#include "flow/cost_logger.hpp"

#include "base/time.hpp"
#include "config.hpp"
#include "flow/db/connection.hpp"
#include "persona_engine/llm/pricing.hpp"

namespace di::flow {

persona_engine::LLMResult CostLoggedClient::invoke(const persona_engine::LLMInvokeArgs& args) {
    const auto start = time::now_ms();
    auto result = inner_.invoke(args);
    const auto latency_ms = time::now_ms() - start;

    auto db = db::get_flow_db();
    const std::optional<persona_engine::TokenUsage> usage =
        result.ok ? result.usage : std::optional<persona_engine::TokenUsage>{};
    // 実モデル: usage.model (worker-pool が transcript から拾った ground truth) を優先し、無ければ args.model。
    const std::optional<std::string> model = (usage && usage->model) ? usage->model : args.model;
    // cost_usd が backend から来ない場合は token × 単価で補完。
    const std::optional<double> cost_usd =
        (usage && usage->cost_usd) ? usage->cost_usd : persona_engine::estimate_cost_usd(model, usage);
    auto opt = [&](std::optional<double> persona_engine::TokenUsage::*field) -> sql::Value {
        if (!usage) return sql::Value();
        return sql::Value((*usage).*field);
    };
    db->run(R"SQL(INSERT INTO llm_call_log
          (flow, session_id, round, turn, role, persona, location, model, backend, latency_ms,
           input_tokens, output_tokens, cache_read_input_tokens, cache_creation_input_tokens, cost_usd,
           prompt, response, created_at)
         VALUES
          (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?))SQL",
            {ctx_.flow, ctx_.sessionId, sql::Value(ctx_.round), sql::Value(ctx_.turn), sql::Value(ctx_.role),
             sql::Value(ctx_.persona), ctx_.location, sql::Value(model),
             ctx_.backend ? *ctx_.backend : config::get_config().llm.backend, latency_ms,
             opt(&persona_engine::TokenUsage::input_tokens), opt(&persona_engine::TokenUsage::output_tokens),
             opt(&persona_engine::TokenUsage::cache_read_input_tokens),
             opt(&persona_engine::TokenUsage::cache_creation_input_tokens), sql::Value(cost_usd), args.prompt,
             result.ok ? sql::Value(result.text) : sql::Value(), time::now_ms()});
    return result;
}

} // namespace di::flow
