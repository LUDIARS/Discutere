#pragma once
// src/flow/dialectic/exchange-prompt.ts の移植: 論拠の応酬 [2] の応答プロンプト + パース (dialectic.md §2.6)。
// 応答者は相手の論拠 1 件に rebut (論拠 1 件) か concede だけを返す。パース失敗は question で全文受理。
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "flow/dialectic/exchange.hpp"
#include "flow/dialectic/store.hpp"
#include "flow/effect_predict.hpp"
#include "flow/personas.hpp"

namespace di::flow::dialectic {

struct ExchangePromptArgs {
    FlowPersona persona;
    FlowStance stance;
    IssueRecord issue;
    std::vector<PositionRecord> positions;
    std::map<std::string, std::string> personaNames;
    /** 定立の論拠から応答対象までの経路 (根が先頭・末尾が応答対象)。 */
    std::vector<ArgumentNode> path;
    std::optional<std::string> voicesBlock;
};
std::string build_exchange_prompt(const ExchangePromptArgs& args);

struct ParsedExchangeTurn {
    /** "rebut" | "concede" | "question" */
    std::string act;
    std::optional<AttackPoint> attack;
    std::optional<InferenceKind> kind;
    std::optional<std::string> data;
    std::optional<std::string> warrant;
    std::string text;
    bool degraded = false;
    std::optional<proxy::ClaimKind> claimKind;
    std::vector<std::string> evidenceIds;
};
Json to_json(const ParsedExchangeTurn& t);
ParsedExchangeTurn parse_exchange_response(const std::string& raw, const WarnFn& warn = {});

} // namespace di::flow::dialectic
