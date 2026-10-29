#pragma once
// tier ルーティング (routing.ts)。易しいタスクは安いモデル、難しいタスクや大入力だけ強いモデルへ。
// ここではどちらの tier を選ぶかの判定だけを行う (モデル ID の解決は呼び出し側)。
#include <optional>
#include <string>
#include <vector>

namespace di::lib::llm_gateway {

/** 'cheap' | 'strong' */
using Tier = std::string;

struct RoutingInput {
    /** タスク種別 (例: "utterance" | "classify" | "summarize" | "converge")。 */
    std::string kind;
    /** 入力のおおよそのトークン数。大きいほど強いモデル寄り。 */
    std::optional<double> inputTokens;
    /** 明示指定があれば最優先。 */
    std::optional<Tier> forceTier;
};

struct RoutingRules {
    /** 無条件で strong にする種別。 */
    std::optional<std::vector<std::string>> strongKinds;
    /** この閾値を超える入力は strong にする。 */
    std::optional<double> strongAboveTokens;
};

/** タスクに対して cheap / strong いずれの tier を使うかを決める。 */
Tier pick_tier(const RoutingInput& input, const std::optional<RoutingRules>& rules = std::nullopt);

} // namespace di::lib::llm_gateway
