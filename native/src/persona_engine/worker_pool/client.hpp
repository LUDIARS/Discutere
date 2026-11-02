#pragma once
// WorkerPoolClient — LLMClient 実装 (src/persona-engine/worker-pool/client.ts)。
// persona-engine の発話生成を「常駐ワーカー (サブスク Lictor セッション)」に委ねる。invoke({personaId}) で
// personaId に対応するワーカーへターンを投げ、発話 callback が戻るまで待って text を返す。
// personaId が未指定 / 未登録ワーカーの場合は ok:false (fallback があれば claude -p へ)。
#include <memory>

#include "persona_engine/llm/client.hpp"
#include "persona_engine/worker_pool/pool.hpp"

namespace di::persona_engine::worker_pool {

class WorkerPoolClient : public LLMClient {
public:
    /**
     * @param pool 常駐ワーカープール
     * @param fallback worker 不在/未起動時に使う既存 LLM (= ClaudeCliClient)。指定時、personaId のワーカーが
     *   起動していなければ claude -p にフォールバックし、model は worker 定義 (delegation) に沿わせる。
     */
    explicit WorkerPoolClient(std::shared_ptr<WorkerPool> pool, LLMClientPtr fallback = nullptr);
    LLMResult invoke(const LLMInvokeArgs& args) override;

private:
    LLMResult invoke_fallback(const LLMInvokeArgs& args, const std::string& worker_id);
    std::shared_ptr<WorkerPool> pool_;
    LLMClientPtr fallback_;
};

} // namespace di::persona_engine::worker_pool
