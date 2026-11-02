#pragma once
// src/flow/personas.ts の移植: 議論フロー用ペルソナ生成 (使い捨て・キャストは flow_session_persona に永続)。
// ロール: ファシリテーター ×1 + 残りを debater / opinion に割当。賛否 (pro/con) はセッション開始時に固定する。
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "base/json.hpp"

namespace di::flow {

/** "facilitator" | "debater" | "opinion" */
using FlowRole = std::string;
/** "pro" | "con" | "neutral" | "opinion" | "reform" | "conservative" (reform / conservative は未来予測の陣営)。 */
using FlowStance = std::string;
/** [0, 1) の乱数 (注入で決定的にする)。 */
using Rng = std::function<double()>;

/** 憑依 (B): データ由来のプールペルソナを「人物像」として演じている状態。 */
struct PersonaPossession {
    /** 憑依対象の露出名 (論者#xxxxxx 等)。 */
    std::string label;
    /** prompt に注入する人物像の説明。 */
    std::optional<std::string> descriptor;
};

/** 代理議論の参加者が持つ人物の束 (proxy/bundle.ts が作る)。 */
struct ProxyPersonaContext {
    std::string label;
    std::string speakerKey;
    std::string genre;
    /** prompt に載せる束の本文。 */
    std::string bundleText;
    /** 束に載った根拠 id (代理が引いてよい id)。 */
    std::vector<std::string> evidenceIds;
    /** 根拠 id → 原文 (引用の検査に使う)。挿入順を保つ。 */
    std::vector<std::pair<std::string, std::string>> evidenceTexts;
};

struct FlowPersona {
    std::string id;
    std::string name;
    FlowRole role;
    /** セッション固定の立場。facilitator=neutral / opinion=opinion / debater=pro|con。 */
    FlowStance stance;
    std::string speechStyle;
    std::vector<std::string> traits;
    std::string model;
    bool isLocal = false;
    std::optional<std::string> valueAxis;
    std::optional<std::vector<std::string>> coreClaims;
    std::optional<PersonaPossession> possession;
    std::optional<ProxyPersonaContext> proxy;
};

Json to_json(const PersonaPossession& p);
Json to_json(const ProxyPersonaContext& p);
Json to_json(const FlowPersona& p);
/** 形が違うフィールドは既定値にする (DB の JSON 列から読む用)。 */
FlowPersona flow_persona_from_json(const Json& j);

/** ロールから導出する既定スタンス。 */
FlowStance role_default_stance(const FlowRole& role);
/** ペルソナのセッション固定スタンス (facilitator → neutral / opinion → opinion / debater は pro|con)。 */
FlowStance persona_stance(const FlowPersona& persona);

/** モデル編成 (spec/feature/flow/model-roster.md)。 */
struct FlowModelRoster {
    std::optional<std::string> facilitator;
    std::vector<std::string> discussants;
};

struct GeneratePersonasArgs {
    int count = 0;
    std::string defaultModel;
    bool isLocal = false;
    Rng rng;
    std::optional<FlowModelRoster> roster;
};

/** discussants が 1 つ以上なら 1 + discussants の数、空なら personaCount。 */
int roster_persona_count(int persona_count, const std::optional<FlowModelRoster>& roster);

/** count 人のペルソナを生成する。count < 1 は std::invalid_argument ("personaCount must be >= 1")。 */
std::vector<FlowPersona> generate_flow_personas(const GeneratePersonasArgs& args);

/** ランダムに 1 人選ぶ (壁打ちの不定期応答でのみ使う)。空なら std::out_of_range。 */
const FlowPersona& pick_random_persona(const std::vector<FlowPersona>& personas, const Rng& rng);

/** 名前プール (姓 + 名)。テスト用に公開。 */
const std::vector<std::string>& full_name_pool();

} // namespace di::flow
