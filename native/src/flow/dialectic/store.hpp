#pragma once
// src/flow/dialectic/store.ts の移植: 弁証法 4 表 (flow_issue / flow_position / flow_tension / flow_synthesis) の永続。
// ドメイン型 + CRUD のみ (LLM なし・判断なし)。
#include <optional>
#include <string>
#include <vector>

#include "base/json.hpp"
#include "flow/dialectic/premise_rules.hpp"
#include "flow/proxy/attribution.hpp"

namespace di::flow::dialectic {

/** "unchallenged" | "challenged" | "defended" | "conceded" */
using GroundState = std::string;

struct Ground {
    std::string id;
    /** データ (事実・事例)。トゥールミンの data。 */
    std::string text;
    GroundState state = "unchallenged";
    std::optional<InferenceKind> kind;
    std::optional<std::string> warrant;
    /** 代理議論のみ: 本人の発言との関係。 */
    std::optional<proxy::ClaimKind> claimKind;
    /** 代理議論のみ: 根拠にした本人の発言 id。 */
    std::optional<std::vector<std::string>> evidenceIds;
};
Json to_json(const Ground& g);
Ground ground_from_json(const Json& j);

struct IssueRecord {
    std::string id;
    std::string sessionId;
    std::string title;
    int ordinal = 0;
    /** "llm" | "paper-gate" */
    std::string source = "llm";
    /** "open" | "concluded" */
    std::string status = "open";
};

struct PositionRecord {
    std::string id;
    std::string issueId;
    std::string personaId;
    /** "pro" | "con" */
    std::string stance = "pro";
    std::string claim;
    std::optional<std::string> qualifier;
    std::vector<Ground> grounds;
    std::vector<std::string> values;
};

/** "facts" | "values" | "means" | "tradeoff" */
using TensionType = std::string;
/** open / resolved / synthesized / compromised / agreed_disagree / unresolved_fact / (旧) agreed / aligned / partially_aligned */
using TensionStatus = std::string;

struct TensionRecord {
    std::string id;
    std::string issueId;
    std::string positionA;
    std::string positionB;
    TensionType type = "values";
    TensionStatus status = "open";
    std::optional<std::string> resolutionNote;
};

struct RatificationEntry {
    std::string positionId;
    /** "accept" | "reject" */
    std::string verdict = "reject";
    std::vector<std::string> preservedIds;
    std::vector<std::string> missing;
    std::string reason;
};
Json to_json(const RatificationEntry& r);
RatificationEntry ratification_from_json(const Json& j);

/** "ratified" | "rejected" | "revising" */
using SynthesisStatus = std::string;

struct SynthesisRecord {
    std::string id;
    std::string tensionId;
    std::vector<std::string> preserves;
    std::vector<std::string> negates;
    std::optional<std::string> elevates;
    /** "aufheben" | "compromise" */
    std::string kind = "compromise";
    std::string text;
    std::vector<RatificationEntry> ratification;
    SynthesisStatus status = "rejected";
    int revision = 0;
};

/** id が空なら uuid を採番する。 */
IssueRecord insert_issue(IssueRecord issue);
void update_issue_status(const std::string& issue_id, const std::string& status);
std::vector<IssueRecord> list_issues(const std::string& session_id);

PositionRecord insert_position(PositionRecord position);
void update_position_grounds(const std::string& position_id, const std::vector<Ground>& grounds);
std::vector<PositionRecord> list_positions(const std::string& issue_id);

TensionRecord insert_tension(TensionRecord tension);
/** resolution_note を変えない版 (TS で resolutionNote が undefined)。 */
void update_tension_status(const std::string& tension_id, const TensionStatus& status);
void update_tension_status(const std::string& tension_id, const TensionStatus& status,
                           const std::optional<std::string>& resolution_note);
std::vector<TensionRecord> list_tensions(const std::string& issue_id);

SynthesisRecord insert_synthesis(SynthesisRecord s);
/** 修正ループの進行 (内容 + 批准 + 状態 + revision) を同一行に上書きする (id 以外の全項目)。 */
void update_synthesis(const std::string& id, const SynthesisRecord& patch);
std::vector<SynthesisRecord> list_syntheses(const std::string& tension_id);

} // namespace di::flow::dialectic
