#include "persona_engine/worker_pool/client.hpp"

#include <exception>
#include <stdexcept>

#include "base/crypto.hpp"
#include "base/strings.hpp"
#include "lapilli/one_shot.hpp"
#include "persona_engine/llm/model_spec.hpp"

namespace di::persona_engine::worker_pool {

WorkerPoolClient::WorkerPoolClient(std::shared_ptr<WorkerPool> pool, LLMClientPtr fallback)
    : pool_(std::move(pool)), fallback_(std::move(fallback)) {
    if (!pool_) throw std::invalid_argument("WorkerPoolClient: pool required");
}

LLMResult WorkerPoolClient::invoke(const LLMInvokeArgs& args) {
    if (args.conversationOnly) return LLMResult::failure("Persistent worker cannot accept external conversation-only requests");
    const std::string worker_id = args.personaId.value_or("");
    if (worker_id.empty()) return LLMResult::failure("worker-pool: personaId 未指定 (ルーティング不能)");

    const bool ready = pool_->has_worker(worker_id) && pool_->is_ready(worker_id);
    if (ready) {
        const std::string req_id = crypto::uuid_v4();
        try {
            auto res = pool_->dispatch(worker_id, DispatchPayload{req_id, args.system.value_or(""), args.prompt});
            // engine の handler は {action,...} JSON 前提。ワーカーは素の発話テキストを返すので、ここで action JSON に
            // 包む (空応答は skip)。usage は action ラップに関係なく LLMResult に載せる (#135)。
            const std::string trimmed = str::trim(res.text);
            if (trimmed.empty())
                return LLMResult::success(json_stringify(Json{{"action", "skip"}, {"reasoning", "worker skip (空応答)"}}), res.usage);
            return LLMResult::success(json_stringify(Json{{"action", "post_utterance"}, {"text", trimmed}}), res.usage);
        } catch (const std::exception& e) {
            // worker 経路で失敗 → fallback があれば claude -p に回す。
            if (fallback_) return invoke_fallback(args, worker_id);
            return LLMResult::failure(std::string("worker-pool: ") + e.what());
        }
    }

    // worker 不在/未起動 → 既存どおり claude -p で動作 (model は delegation 準拠)。
    if (fallback_) return invoke_fallback(args, worker_id);
    return LLMResult::failure("worker-pool: worker '" + worker_id + "' 未起動 かつ fallback 無し");
}

LLMResult WorkerPoolClient::invoke_fallback(const LLMInvokeArgs& args, const std::string& worker_id) {
    const auto cfg = pool_->get_worker_config(worker_id);
    const std::optional<std::string> raw_model = cfg ? std::optional<std::string>(cfg->model) : args.model;
    // fallback は claude -p。Claude の役割名・明示 ID を保持し、Codex ワーカーには Claude の既定値を使う。
    LLMInvokeArgs next = args;
    const auto model = parse_model_spec(raw_model).model;
    if ((!cfg || cfg->provider == "claude") &&
        (lapilli::one_shot::role_provider(model) == lapilli::one_shot::Provider::claude ||
         str::starts_with(model, "claude-")))
        next.model = raw_model;
    else
        next.model.reset();
    return fallback_->invoke(next);
}

} // namespace di::persona_engine::worker_pool
