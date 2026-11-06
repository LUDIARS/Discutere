#pragma once
// w5r_gate_tests.cpp を Praesidium の静的解析 (1 単位 60 秒) に収まるよう分けた各パートが共有する準備。
// w5-R: src/flow/information-gate-runner.ts と src/flow/web/flow-prep.ts の移植の検証 (TS テスト無し)。
// golden: gateBeforeFlow の対象外の分岐と LLM 評価 1 回 (呼び出し列・外部の声の検索・log・コストログ)、
// resolveDebatabilityGate、buildAutoCrawlSpec / buildLearningCrawl (config の env 違い)。
// 加えて prepare_information_before_flow の 機密 / 情報ゲート / 旧 autoCrawl の分岐を偽の collector で確かめる (ネットワークに出ない)。
#include "check.hpp"
#include "w4q_support.hpp"
#include "w5r_support.hpp"

#include <atomic>

#include "base/strings.hpp"

#include "flow/information_gate_runner.hpp"
#include "flow/web/flow_prep.hpp"

using namespace di;
using namespace di::flow;

namespace w5r_gate_support {

void set_envs(const Json& envs) {
    for (auto it = envs.begin(); it != envs.end(); ++it) env::set_for_test(it.key(), std::string_view(it.value().get<std::string>()));
    config::reset_config_for_test();
}
void unset_envs(const Json& envs) {
    for (auto it = envs.begin(); it != envs.end(); ++it) env::set_for_test(it.key(), std::nullopt);
    config::reset_config_for_test();
}

} // namespace w5r_gate_support
