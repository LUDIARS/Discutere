#pragma once
// src/flow/persona-history-evidence.ts の移植: 外部の発言履歴から作った人物像の根拠 (期限つき) の検証と保存。
// zod の strict スキーマと同じ検査 (余分なキー・型違い・範囲外は拒否) を手書きで行う。
#include <cstdint>
#include <optional>
#include <string>

#include "base/json.hpp"
#include "base/sqlite.hpp"

namespace di::flow {

struct PersonaHistoryEvidence {
    int schemaVersion = 1;
    std::string source;  // steam | youtube
    double generatedAt = 0;
    double expiresAt = 0;
    double windowStart = 0;
    double firstObservedAt = 0;
    double lastObservedAt = 0;
    double sampleCount = 0;
    double observedMonths = 0;
    std::string coverage;  // partial | target-window-observed
};

/**
 * value が nullopt (undefined) なら nullopt。形が違えば std::invalid_argument、
 * 期限切れ・時刻の前後関係が不正なら std::runtime_error ("Invalid or expired history evidence")。
 */
std::optional<PersonaHistoryEvidence> parse_history_evidence(const std::optional<Json>& value, double now_ms);

Json to_json(const PersonaHistoryEvidence& e);

/** flow_persona_history に upsert する (evidence が無ければ何もしない)。 */
void save_history_evidence(sql::Database& db, const std::string& user_id, const std::optional<PersonaHistoryEvidence>& evidence);

/** 期限切れの派生プロフィールを議論に残さない (flow_persona と flow_persona_history から消す)。 */
void purge_history_personas(sql::Database& db, double now_ms);

} // namespace di::flow
