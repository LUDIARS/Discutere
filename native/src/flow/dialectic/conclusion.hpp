#pragma once
// src/flow/dialectic/conclusion.ts の移植: dialectic の結論 (respec 06 §2「結論」, dialectic.md §6)。
//
// v2 (P3): 結論本文は決着表 (settlement_table) をコードで写し、LLM は要約部だけを書く。投票の勝者と
// 直近 20 発話は渡さない。結論があるかは「決着が 1 件以上あるか」で決める (投票の成否に依らない)。
// converge 入力を「直近 20 発話」でなく論証状態から組み立てる:
//   - issue ごとの決着表 (止揚 / 論破 / 結論なし の 3 区分 + 結論なしの理由 — §4.5)
//   - 未解決の事実問題 (unresolved_fact の resolution_note) — **結論本文に必ず明記** (誠実性)
//   - レンズ投票の集中度
//
// 永続 (flow_conclusion / タイトルキャッシュ / 結論キャッシュ) は既存 generate_conclusion を
// 流用する (学習ビュー・議論一覧が dialectic セッションもそのまま表示できる)。
// 未解決の事実問題は annotation としても併記するため、LLM が無視しても結論文に必ず残る。
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "flow/conclusion.hpp"
#include "flow/director.hpp"
#include "flow/dialectic/exchange_end.hpp"
#include "flow/dialectic/store.hpp"
#include "flow/paper_markdown.hpp"
#include "flow/vote.hpp"
#include "persona_engine/llm/client.hpp"

namespace di::flow::dialectic {

/** issue 1 件の決着スナップショット (driver が組み立てる)。 */
struct IssueSettlement {
    IssueRecord issue;
    std::vector<PositionRecord> positions;
    std::vector<TensionRecord> tensions;
    /** tension ごとの最終 Synthesis (無ければ空)。 */
    std::vector<SynthesisRecord> syntheses;
    /** 論破で決着した場合の判定文 (Tension を立てない)。論破でなければ nullopt。 */
    std::optional<std::string> refutation;
    /** 双方の論拠がすべて崩れて結論なしにした理由 (Tension を立てない)。無ければ nullopt。 */
    std::optional<std::string> undecided;
    /** 論拠の応酬の終わり方 (応酬しなかった論点は nullopt)。 */
    std::optional<ExchangeEnd> exchangeEnd;
};

/**
 * issue 決着を RoundSummary に射影する (round = issue ordinal)。
 * 既存の converge プロンプト「# 各ラウンドの要約」節とペーパー経過表示の両方がこれを読む。
 */
RoundSummary settlement_to_round_summary(const IssueSettlement& s, const std::map<std::string, std::string>& persona_names,
                                         std::optional<double> winner_share);

/** 全 settlement から未解決の事実問題を集める。 */
std::vector<std::string> collect_unresolved_facts(const std::vector<IssueSettlement>& settlements);

struct ConcludeDialecticArgs {
    std::string theme;
    std::string sessionId;
    std::string paperId;
    std::string flow;
    std::vector<IssueSettlement> settlements;
    /** 議論しなかった論点 (収束での打ち切り)。決着表に「未討議」で載せる。 */
    std::vector<IssueRecord> undiscussed;
    std::map<std::string, std::string> personaNames;
    std::vector<FlowUtteranceRecord> allUtterances;
    std::vector<VoteResult> voteResults;
    /** 批准済み aufheben の統合文 (折衷は含めない)。flow_conclusion.aufhebung_json に永続。 */
    std::vector<std::string> ratifiedAufheben;
    /** 議論適性: 低のまま強行した場合の注記 (09 と同じ扱い)。 */
    std::optional<std::string> debatabilityNote;
    /** 要約に使うモデル (空は既定)。 */
    std::optional<std::string> model;
    /** flow_conclusion.engine_version (v2 P10)。 */
    std::optional<std::string> engineVersion;
    WarnFn warn;
};

/**
 * dialectic の結論を生成・永続する。本文 = 要約 (LLM、失敗時は定型の断り) + 決着表 + 未解決の事実問題 + 注記。
 * concluded は決着した論点が 1 件以上あるか。保存は persist_flow_conclusion (rounds と共用)。
 */
ConclusionResult conclude_dialectic(persona_engine::LLMClient& llm, const ConcludeDialecticArgs& args);

} // namespace di::flow::dialectic
