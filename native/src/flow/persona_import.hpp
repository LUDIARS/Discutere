#pragma once
// src/flow/persona-import.ts の移植: Voluptas 匿名プロフィール → flow_persona の取込 (@spec インポート (Vo → Di))。
// 入力は呼び出し側が上限つきで読んだ JSON (parse_persona_document 等)。このモジュールはファイルを読まない。
// 検査 (仮名 ID の形・20 次元 0..1・版・正準の嗜好軸) は TS と同じ文言で std::invalid_argument を投げる。
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "base/json.hpp"
#include "flow/persona_history_evidence.hpp"
#include "flow/persona_pool.hpp"

namespace di::flow {

struct VoluptasPersonaPayload {
    std::string pseudoId;
    std::vector<double> affectVector;
    double vectorSpecVersion = 1;
    std::vector<std::string> traits;
    std::vector<std::pair<std::string, double>> preferenceAxes;
    PersonaAttributes attributes;
    std::vector<PersonaAversion> aversions;
    std::vector<PersonaMechanicReaction> mechanicReactions;
    std::optional<double> exportSpecVersion;
    std::optional<PersonaHistoryEvidence> historyEvidence;
};

struct PersonaImportSummary {
    double imported = 0;
    double inserted = 0;
    double updated = 0;
    double skipped = 0;
    /** 理由 → 件数 (出現順)。 */
    std::vector<std::pair<std::string, double>> skipReasons;
    bool dryRun = false;
};

Json to_json(const VoluptasPersonaPayload& p);
Json to_json(const PersonaImportSummary& s);

/** 1 件を検査して読む。形が違えば std::invalid_argument (TS の TypeError と同じ文言)。now_ms は履歴根拠の期限判定に使う。 */
VoluptasPersonaPayload parse_voluptas_persona_payload(const Json& value, double now_ms);
VoluptasPersonaPayload parse_voluptas_persona_payload(const Json& value);

/** 配列か { personas: [] } を受ける。それ以外は std::invalid_argument。 */
Json parse_voluptas_persona_batch(const Json& value);

/** 取込の理由コード (export-spec / vector-spec / affect-vector / pseudo-id / invalid-payload)。 */
std::string persona_import_reason_code(const std::string& message);

struct ImportVoluptasOptions {
    bool dryRun = false;
    /** NDJSON の壊れた行の数 (取込前に落ちているので skip に合算する)。 */
    double invalidJsonLines = 0;
};

PersonaImportSummary import_voluptas_personas(const Json& value, const ImportVoluptasOptions& options = {});

} // namespace di::flow
