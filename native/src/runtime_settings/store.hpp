#pragma once
// Runtime 設定ストア (src/runtime-settings/store.ts の移植)。
// boot 時の既定値をデフォルトに、SQLite の override があれば差し替えて返す。facilitator の収束トリガー /
// ワーカーの役割プロンプト / debate ルール instructions / コスト relay を再起動なしで Web UI から調整する。
//
// - 保存先は persona-engine の SQLite (peDb) に同居する `runtime_settings` テーブル (TS と同じ DDL)。
// - 値は JSON 文字列。key 体系:
//     facilitator             → FacilitatorTuning の実効値 (JSON)
//     costRelay               → CostRelaySettings の実効値 (JSON)
//     rolePrompt:<role>       → 役割ガイダンス本文 (string)
//     ruleInstruction:<id>    → debate ルール instructions 本文 (string)
// - in-memory cache を持ち、set 時に更新 (mutex で守る)。別プロセスが書く想定は無い (単一 bot)。
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/json.hpp"
#include "base/sqlite.hpp"

namespace di::runtime_settings {

/** "default" | "aufhebung-only" */
using ConvergePolicy = std::string;

/** facilitator の収束トリガー設定。数値は JS の number と同じく double で持つ。 */
struct FacilitatorTuning {
    double tickMs = 0;           // tick 周期 ms
    double idleGapMs = 0;        // 発言が止まってから介入するまでの idle ms
    double maxPersonas = 0;      // 参加 persona がこの数を超えたら収束
    double aufhebungTarget = 0;  // 止揚がこの数たまったら収束
    ConvergePolicy convergePolicy = "default";
    bool operator==(const FacilitatorTuning&) const = default;
};
Json to_json(const FacilitatorTuning& t);

/** コスト relay 設定 (Anatomia / Concordia への push)。 */
struct CostRelaySettings {
    bool enabled = false;
    std::string anatomiaUrl;
    std::string concordiaUrl;
    std::string service;
    bool operator==(const CostRelaySettings&) const = default;
};
Json to_json(const CostRelaySettings& s);

/** 既定群。rolePrompts / ruleInstructions は TS の Record (挿入順。整数形のキーは先頭に昇順で並ぶ)。 */
struct RuntimeSettingsDefaults {
    FacilitatorTuning facilitator;
    std::vector<std::pair<std::string, std::string>> rolePrompts;
    std::vector<std::pair<std::string, std::string>> ruleInstructions;
    CostRelaySettings costRelay;
};

/** UI 表示用: デフォルト / override / 実効値 の 3 点セット。 */
struct OverrideView {
    std::string key;
    std::string default_value;              // JSON では "default"
    std::optional<std::string> override_value;  // JSON では "override" (null あり)
    std::string effective;
};
Json to_json(const OverrideView& v);

/** override (部分 JSON) をデフォルトにマージする (pure)。数値は下限クランプ、policy は enum 検証、不正値はデフォルト。 */
FacilitatorTuning merge_facilitator_tuning(const FacilitatorTuning& def, const Json& override_json);
/** cost relay override (部分 JSON) をデフォルトにマージする (pure)。URL は trim、文字列以外はデフォルト。 */
CostRelaySettings merge_cost_relay(const CostRelaySettings& def, const Json& override_json);

/** ストアの interface (tuning-routes や facilitator は これを通して読む / テストで差し替えられる)。 */
class RuntimeSettingsStore {
public:
    virtual ~RuntimeSettingsStore() = default;
    virtual FacilitatorTuning get_facilitator_tuning() = 0;
    /** patch は Partial<FacilitatorTuning> 相当の JSON object。実効値を返す。 */
    virtual FacilitatorTuning set_facilitator_tuning(const Json& patch) = 0;
    virtual std::string get_role_prompt(const std::string& role) = 0;
    virtual std::vector<OverrideView> list_role_prompts() = 0;
    /** nullopt / 空白だけならデフォルトに戻す。 */
    virtual void set_role_prompt(const std::string& role, const std::optional<std::string>& text) = 0;
    virtual std::string get_rule_instruction(const std::string& rule_id) = 0;
    virtual std::vector<OverrideView> list_rule_instructions() = 0;
    virtual void set_rule_instruction(const std::string& rule_id, const std::optional<std::string>& text) = 0;
    virtual CostRelaySettings get_cost_relay() = 0;
    virtual CostRelaySettings set_cost_relay(const Json& patch) = 0;
    virtual void reload() = 0;
};

/** db 上に runtime_settings を確保してストアを作る。 */
std::shared_ptr<RuntimeSettingsStore> create_runtime_settings_store(sql::DatabasePtr db, RuntimeSettingsDefaults defaults);

} // namespace di::runtime_settings
