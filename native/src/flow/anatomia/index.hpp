#pragma once
// src/flow/anatomia/index.ts の移植: Anatomia 連携の入口 (取得 + 精製のオーケストレーション)。
//   get_anatomia_mechanics(source) = fetch_anatomia_domains (CLI) → refine_domains_to_mechanics (LLM)
#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "flow/anatomia/client.hpp"
#include "flow/anatomia/refine.hpp"
#include "flow/anatomia/types.hpp"

namespace di::flow::anatomia {

struct GetAnatomiaMechanicsDeps {
    std::string binPath;
    std::optional<bool> autoDraft;
    std::optional<std::chrono::milliseconds> timeout;
    persona_engine::LLMClient* llm = nullptr;
    std::optional<std::string> refineModel;
    std::optional<std::string> theme;
    AnatomiaCliRunner runner;
    std::function<void(const std::string&)> warn;
};

/** 取得失敗 (bin 不在 / CLI 非 0 / JSON 不正) は fetch_anatomia_domains が std::runtime_error を投げる (fail-fast)。 */
std::vector<MechanicSummary> get_anatomia_mechanics(const AnatomiaSource& source, const GetAnatomiaMechanicsDeps& deps = {});

/** リクエスト由来の文字列ペアから AnatomiaSource を組み立てる (project 優先・両空は nullopt)。 */
std::optional<AnatomiaSource> resolve_anatomia_source(const std::optional<std::string>& project, const std::optional<std::string>& repo_path);

} // namespace di::flow::anatomia
