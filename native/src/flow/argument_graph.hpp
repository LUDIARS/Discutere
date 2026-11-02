#pragma once
// src/flow/argument-graph.ts の移植: 論証グラフ (respec 05 §3 / dialectic.md §0)。
// dialectic エンジンの発話 (act/target 付き) から「誰がどの主張に反論し、何が未応答か」を決定的に導出する。
// 純関数 + 小さな状態クラス・LLM 不使用。永続は flow_utterance (act/target_id) からいつでも再構築できる。
//
// 遷移規則 (claim/ground 単位):
//   claim ── rebut ──> challenged ── support(同陣営, target=rebut) ──> defended
//                                 └─ concede(被反論側) ─────────────> conceded
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace di::flow {

/** 会話行為 (dialogue act): "claim" | "support" | "rebut" | "concede" | "question" | "synthesize"。 */
using DialogueAct = std::string;

/** DB / LLM 由来の act 文字列を正規化する。null / 未知値は claim 扱い (後方互換)。 */
DialogueAct coerce_act(const std::optional<std::string>& act);

struct GraphUtterance {
    std::string id;
    std::string personaId;
    DialogueAct act;
    /** 応答先 utterance id (claim/question は null 可)。 */
    std::optional<std::string> targetId;
    /** セッション通しのターン番号。 */
    double turn = 0;
};

/** "challenged" | "defended" | "conceded" */
using RebuttalState = std::string;

struct RebuttalEdge {
    std::string rebutId;
    std::string targetId;
    std::string byPersonaId;
    /** 反論された claim の持ち主 (= 応答義務を負う persona)。ターゲット不明なら nullopt。 */
    std::optional<std::string> targetPersonaId;
    RebuttalState state;
};

struct ConcessionEdge {
    std::string concedeId;
    std::string rebutId;
    std::string targetId;
};

struct ClaimNodeView {
    std::string id;
    std::string personaId;
    double turn = 0;
};

/** 論証グラフ。apply で状態遷移し、計測メソッドで収束判定・スケジューラの材料を返す。決定的 (入力順のみに依存)。 */
class ArgumentGraph {
public:
    /** 発話を 1 つ適用し状態遷移する (未知 target は無視して受理 = degrade)。 */
    void apply(const GraphUtterance& u);

    /** rebut されて defend も concede もされていない辺 (未応答反論)。挿入順。 */
    std::vector<RebuttalEdge> unresolved_rebuttals() const;
    /** 誰も触れていない claim の id 列 (挿入順)。 */
    std::vector<std::string> unanswered_claims() const;
    /** concede 済みの辺 (止揚の原材料)。 */
    std::vector<ConcessionEdge> concessions() const;
    /** turn より後の新規 claim 数。 */
    int new_claims_since(double turn) const;
    std::vector<ClaimNodeView> claim_nodes() const;
    /** claim を対象にした rebut 辺の数 (状態問わず)。 */
    int edge_count_for(const std::string& claim_id) const;
    /** claim を対象にした rebut 辺 (状態問わず・挿入順)。 */
    std::vector<RebuttalEdge> rebuttals_targeting(const std::string& claim_id) const;
    /** rebut 辺を id で引く。 */
    std::optional<RebuttalEdge> rebuttal_by_id(const std::string& rebut_id) const;

private:
    struct ClaimNode {
        std::string id;
        std::string personaId;
        double turn = 0;
        bool touched = false;
    };
    void touch(const std::string& target_id);
    ClaimNode* find_claim(const std::string& id);
    RebuttalEdge* find_rebuttal(const std::string& id);
    void set_claim(ClaimNode node);
    void set_rebuttal(RebuttalEdge edge);

    // Map の挿入順を保つ: 値は vector、id → 位置の索引を別に持つ (同じ id の再設定は位置を保って上書き)。
    std::vector<ClaimNode> claims_;
    std::unordered_map<std::string, std::size_t> claim_index_;
    std::vector<RebuttalEdge> rebuttals_;
    std::unordered_map<std::string, std::size_t> rebuttal_index_;
    std::vector<ConcessionEdge> conceded_;
};

/** flow_utterance 行の射影 (act / target_id は NULL 可)。 */
struct UtteranceRow {
    std::string id;
    std::string persona_id;
    std::optional<std::string> act;
    std::optional<std::string> target_id;
    double turn = 0;
};

/** flow_utterance 行からグラフを再構築する (act=NULL の旧行は claim 扱い)。 */
ArgumentGraph build_graph_from_rows(const std::vector<UtteranceRow>& rows);

} // namespace di::flow
