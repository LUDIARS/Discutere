#pragma once
// src/flow/foresight/phase.ts の移植: 未来予測アドバイザリー (spec/feature/flow/foresight.md) の進行。
// 結論を変えない参考として、本議論 (議論 / 改善、rounds / dialectic どちらのエンジンでも) の結論の後に、
//   [F1] 進行役が 前提事実 → 理論の当てはめ → 破壊可能な一手 + 起きること + リスク を提案
//   [F2] 改革派 (賛同) と 保守派 (否定) が 成立条件 → 反証 → 確かめ方 の順に交互に論じる
//   [F3] 記録係が 未解決の前提 + 3 / 5 / 10 年時点の条件付きシナリオ (+ ビジネス分析) をまとめる
// を 1 ラウンド分の発話として流す。発話は本議論と同じ flow_utterance に永続し onUtterance で通知する。
// 提案と予測はペーパー本文末尾の「# 未来予測アドバイザリー (参考)」節にも焼く。
//
// LLM 失敗は発話にエラーを出して続ける (OVERVIEW §10)。提案が出なければ以降を行わない。
// 結果の status で 完了 / 一部失敗 / 失敗 を返し、Web の状態表示に使う。
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "flow/director.hpp"
#include "flow/foresight/prompts.hpp"
#include "persona_engine/llm/client.hpp"

namespace di::flow::foresight {

/** Discord の 1 投稿 (webhook は 1900 字で切る) に収める上限 (分割番号の分を空けている)。 */
inline constexpr std::size_t POST_LIMIT = 1780;

/** ペーパー本文の読み書き (「# 未来予測アドバイザリー (参考)」節を焼く)。 */
struct PaperBodyAccess {
    std::function<std::optional<std::string>()> get;
    std::function<void(const std::string&)> set;
};

struct ForesightPhaseArgs {
    std::string theme;
    std::string sessionId;
    std::string paperId;
    std::string flow;
    /** 本議論の発話 (話者の引き継ぎ元)。 */
    std::vector<FlowUtteranceRecord> utterances;
    /** 本議論の結論 (結論なしなら空でよい)。 */
    std::string conclusion;
    /** 本議論の文脈 (ペーパー本文 + 外部の声)。提案と予測に渡す。 */
    std::optional<ForesightContext> context;
    /** 改革派 / 保守派の発言回数 (往復単位に揃える)。 */
    double turns = 0;
    /** 提案と議論に使うモデル (nullopt なら LLM の既定)。 */
    std::optional<std::string> model;
    /** 予測に使うモデル (nullopt なら model)。 */
    std::optional<std::string> forecastModel;
    /** 発話の永続 + 通知 (本議論と同じ経路)。 */
    std::function<void(const FlowUtteranceRecord&)> commit;
    std::optional<PaperBodyAccess> paperBody;
    WarnFn log;
    WarnFn warn;
};

/** "done" = 全部出た / "degraded" = 一部の発言か予測が失敗 / "failed" = 提案が出ず止めた。 */
using ForesightPhaseStatus = std::string;

struct ForesightPhaseResult {
    std::string proposal;
    std::string forecast;
    std::vector<FlowUtteranceRecord> utterances;
    ForesightPhaseStatus status;
    /** 失敗した段の数 (議論の発言 + 予測 + ペーパー追記)。 */
    int failures = 0;
};

/**
 * 長文を行単位で limit 以下に分ける (1 行が長すぎる時だけ文字数で切る)。
 * 文字数はコードポイントで数え、サロゲートペアを割らない。limit 0 は 1 として扱う。
 */
std::vector<std::string> split_for_post(const std::string& text, std::size_t limit = POST_LIMIT);

/** 複数に分けた時だけ「(n/m)」を末尾に付ける (どの投稿が 1 発言の続きか分かるように)。 */
std::vector<std::string> label_parts(const std::vector<std::string>& chunks);

/** 未来予測フェーズを実行する。進行役以外の話者が 2 人未満なら nullopt (行わない)。commit は必須。 */
std::optional<ForesightPhaseResult> run_foresight_phase(persona_engine::LLMClient& llm, const ForesightPhaseArgs& args);

} // namespace di::flow::foresight
