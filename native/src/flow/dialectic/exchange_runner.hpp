#pragma once
// src/flow/dialectic/exchange-runner.ts の移植: 論拠の応酬 [2] の進行 (dialectic.md §2.6)
// — driver から 1 論点ぶんを切り出した進行台本。
//
//   1. 応答待ちの論拠を幅優先で 1 件取り出す (exchange)
//   2. 相手陣営の発言数最少ペルソナが「反論 (論拠 1 件)」か「認める」で答える
//   3. 反論は前提ルールのゲート (rule_check) を通ったときだけ論拠として積む
//   4. 応答待ちが無くなる (= 論拠が落ち着く) か応答ターン上限で終わる
// 終わったら定立の論拠の決着を Position の根拠状態へ書き戻す (goal の論破判定が読む)。
//
// 永続・通知・論証グラフの更新は driver から渡された commit / applyGraph に任せる。
// 呼んだスレッドで同期に進める (LLM の待ちもこのスレッド)。
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "flow/argument_graph.hpp"
#include "flow/director.hpp"
#include "flow/dialectic/exchange.hpp"
#include "flow/dialectic/store.hpp"
#include "flow/dialectic/turn_prompt.hpp"
#include "flow/effect_predict.hpp"
#include "flow/personas.hpp"
#include "flow/proxy/attribution.hpp"
#include "persona_engine/llm/client.hpp"

namespace di::flow::dialectic {

struct RecordArgs {
    FlowPersona persona;
    int round = 0;
    int turn = 0;
    std::string text;
    std::optional<DialogueAct> act;
    std::optional<std::string> targetId;
    bool isError = false;
    /** 代理議論のみ: 検査済みの帰属区分と根拠 id。 */
    std::optional<proxy::ClaimKind> claimKind;
    std::optional<std::vector<std::string>> evidenceIds;
};

/**
 * ReadonlyMap<Side, PositionRecord> (挿入順を保つ)。Position は driver が持つ実体を指す
 * (TS と同じく書き戻しが driver の positions にも見える)。
 */
class PositionsBySide {
public:
    void set(const Side& side, PositionRecord* position);
    PositionRecord* get(const Side& side) const;
    /** 挿入順の Position (TS の [...map.values()])。 */
    std::vector<PositionRecord*> values() const;

private:
    std::vector<std::pair<Side, PositionRecord*>> entries_;
};

/** 発言者・ターンごとの with_cost_log 済み LLM を作る。 */
using TurnLlmFactory = std::function<std::unique_ptr<persona_engine::LLMClient>(const FlowPersona& persona, int turn)>;
using RuleCheckLlmFactory = std::function<std::unique_ptr<persona_engine::LLMClient>(int turn)>;

struct Debaters {
    FlowPersona pro;
    FlowPersona con;
    const FlowPersona& of(const Side& side) const { return side == "con" ? con : pro; }
};

struct GroundExchangeArgs {
    IssueRecord issue;
    int round = 0;
    /** 定立の投稿まで終えた時点のターン番号 (この続きから数える)。 */
    int turn = 0;
    GroundExchange* exchange = nullptr;
    const PositionsBySide* positionBySide = nullptr;
    std::vector<FlowPersona> speakers;
    /** 陣営に発言者がいないときの代表 (ensure_debaters の pro/con)。 */
    Debaters debaters;
    FlowPersona facilitator;
    std::map<std::string, std::string> personaNames;
    const std::map<std::string, UtteranceDigest>* utteranceIndex = nullptr;
    const std::map<std::string, int>* speakCounts = nullptr;
    std::string paperSystem;
    std::string voicesBlock;
    /** 応答ターン上限 (flow.dialectic.exchangeTurns)。 */
    int turnCap = 0;
    bool ruleCheck = true;
    std::optional<std::string> judgeModel;
    TurnLlmFactory turnLlm;
    RuleCheckLlmFactory ruleCheckLlm;
    persona_engine::LLMClient* facilitatorLlm = nullptr;
    std::function<FlowUtteranceRecord(const RecordArgs&)> makeRecord;
    std::function<void(const FlowUtteranceRecord&)> commit;
    std::function<void(const FlowUtteranceRecord&, const DialogueAct&, const std::optional<std::string>&)> applyGraph;
    WarnFn log;
    WarnFn warn;
};

/** 陣営の発言者 (発言数最少・同点は入力順)。いなければ nullptr。 */
const FlowPersona* pick_side_speaker(const std::vector<FlowPersona>& speakers, const Side& side,
                                     const std::map<std::string, int>& speak_counts);

/** 応酬を回し、最後のターン番号を返す。必須の引数 (exchange 等) が無ければ std::invalid_argument。 */
int run_ground_exchange(const GroundExchangeArgs& args);

/** 定立の論拠の決着を Position の根拠状態へ書き戻す (変化した Position だけ永続)。 */
void write_back_ground_states(const GroundExchange& exchange, const PositionsBySide& position_by_side);

} // namespace di::flow::dialectic
