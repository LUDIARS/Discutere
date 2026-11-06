#pragma once
// src/flow/dialectic/driver.ts の移植: dialectic 位相ドライバ (respec 05/06, dialectic.md §3) — 論証状態機械の進行台本。
//
//   [0] 論点分解 (paperOverride.issues があれば再分解しない)
//   [1] 定立 (issue × pro/con debater の Position 生成。論拠 1〜3 件を陣営交互に 1 件ずつ投稿)
//   [2] 論拠の応酬 (相手の論拠に「反論 = 論拠 1 件」か「認める」で答える。落ち着いたらまとめ — §2.6)
//   [3] 矛盾特定 (型分類。facts → RAG 解消 / 失敗は unresolved_fact)
//   [4] 結論 (止揚 / 論破 / 結論なし の 3 区分 — §4.5。止揚は型別テンプレ生成 → 折衷ゲート → 敵対的批准 → 修正ループ ≤2)
//   [5] 計測型収束 (2/3 シグナル + aufhebungTarget 上限キャップ)
//
// 状態遷移・スケジューリング・収束判定はすべて決定的なコード。LLM は生成と敵対的批准のみ。
// FlowDirectorResult 互換の結果を返し (dispatch の戻り型は不変)、永続は既存関数
// (setup_flow_paper→persist_paper / persist_utterance / generate_conclusion 系) を流用するため、
// 学習ビュー・議論一覧・Discord/Web の露出面は rounds エンジンと同じ経路で動く。
//
// 呼んだスレッドで最後まで同期に進める。C++ 版の追加: options.state (FlowRunState) を渡すと
// 段階 (setup / cast / round / vote / conclusion / finalize / done) と 論点番号・ターン・発話数を別スレッドから読め、
// request_stop() で次の区切り (論点の開始・発話の永続の直後・投票・結論の前) に FlowStopped を投げて止まる。
#include <string>
#include <vector>

#include "flow/director.hpp"
#include "flow/personas.hpp"
#include "flow/tags.hpp"
#include "persona_engine/llm/client.hpp"

namespace di::flow::dialectic {

struct EnsuredDebaters {
    FlowPersona pro;
    FlowPersona con;
    std::vector<FlowPersona> cast;
};

/**
 * pro/con の debater を確保する (不足時は非 facilitator を昇格して degrade。warn 明示)。
 * 昇格できる人がいなければ std::runtime_error。
 */
EnsuredDebaters ensure_debaters(const std::vector<FlowPersona>& cast, const WarnFn& warn);

/**
 * dialectic エンジンで議論フローを実行する。run_flow (rounds) と同じ FlowDirectorResult を返す (dispatch / 露出面の互換)。
 * options.evaluateRound / rounds / turnsPerRound / possess は使わない (TS と同じ)。
 */
FlowDirectorResult run_dialectic_flow(persona_engine::LLMClient& llm, const std::string& theme, const std::vector<FlowTag>& tags,
                                      const FlowRunOptions& options);

/** dispatch 用: dialectic エンジンの議論フロー。 */
FlowDirectorResult run_dialectic_discussion_flow(persona_engine::LLMClient& llm, const std::string& theme,
                                                 const std::vector<FlowTag>& tags, const FlowDirectorDeps& deps);

/** dispatch 用: dialectic エンジンの改善フロー (決着はレンズ投票 — 06 §2)。 */
FlowDirectorResult run_dialectic_improvement_flow(persona_engine::LLMClient& llm, const std::string& theme,
                                                  const std::vector<FlowTag>& tags, const FlowDirectorDeps& deps);

} // namespace di::flow::dialectic
