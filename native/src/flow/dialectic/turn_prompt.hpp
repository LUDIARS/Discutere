#pragma once
// src/flow/dialectic/turn-prompt.ts の移植: dialectic エンジンの発話プロンプト + パース (respec 05 §1)。
// ペルソナターンは露出用の口語文と論証上の「手」(act) を同時に返す。パース失敗は act=claim で全文受理。
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "flow/argument_graph.hpp"
#include "flow/dialectic/premise_rules.hpp"
#include "flow/dialectic/store.hpp"
#include "flow/effect_predict.hpp"
#include "flow/personas.hpp"

namespace di::flow::dialectic {

struct UtteranceDigest {
    std::string id;
    std::string personaName;
    std::string text;
};

struct DialecticTurnPromptArgs {
    FlowPersona persona;
    FlowStance stance;
    IssueRecord issue;
    std::vector<PositionRecord> positions;
    std::vector<RebuttalEdge> unresolvedRebuttals;
    std::map<std::string, UtteranceDigest> utterances;
    std::map<std::string, std::string> personaNames;
    std::vector<DialogueAct> allowedActs;
    std::optional<std::string> targetUtteranceId;
    std::optional<std::string> nomination;
    std::optional<std::string> voicesBlock;
};

std::string render_position_digest(const PositionRecord& p, const std::string& persona_name);
std::string build_dialectic_turn_prompt(const DialecticTurnPromptArgs& args);

struct ParsedTurn {
    DialogueAct act;
    std::optional<std::string> targetId;
    std::optional<std::string> groundId;
    std::optional<AttackPoint> attack;
    std::optional<std::string> data;
    std::optional<std::string> warrant;
    std::string text;
    bool degraded = false;
};
Json to_json(const ParsedTurn& t);

struct ParseTurnOptions {
    std::vector<DialogueAct> allowedActs;
    std::set<std::string> validTargets;
    std::optional<std::string> defaultTargetId;
    WarnFn warn;
};
ParsedTurn parse_turn_response(const std::string& raw, const ParseTurnOptions& opts);

} // namespace di::flow::dialectic
