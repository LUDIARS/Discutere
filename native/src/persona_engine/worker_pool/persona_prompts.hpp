#pragma once
// 常駐ワーカーの standing persona prompt 生成 (src/persona-engine/worker-pool/persona-prompts.ts)。
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "persona_engine/types.hpp"
#include "persona_engine/worker_pool/types.hpp"

namespace di::persona_engine::worker_pool {

/** 既定の 8 キャスト。spec §3。 */
const std::vector<WorkerConfig>& DEFAULT_WORKERS();
/** 役割別の既定ガイダンス (runtime-settings の rolePrompts デフォルトに渡す)。 */
const std::map<std::string, std::string>& ROLE_GUIDANCE_DEFAULTS();

using RolePromptResolver = std::function<std::optional<std::string>(const std::string& role)>;
/** 役割ガイダンスの runtime override 解決関数を差す (nullptr で解除)。スレッド安全。 */
void set_role_prompt_resolver(RolePromptResolver fn);

/** ワーカー定義 → persona-engine の PersonaSeed (persona id = worker id)。 */
std::vector<PersonaSeed> build_worker_persona_seeds(const std::vector<WorkerConfig>& workers);
/** standing prompt を組み立てる。 */
std::string build_standing_prompt(const WorkerConfig& worker);

} // namespace di::persona_engine::worker_pool
