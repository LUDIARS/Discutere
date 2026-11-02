#pragma once
// persona bridge の本人確認アサーションの再送防止 (src/persona-bridge/assertion-replay-store.ts)。
// SPEC-PERSONA-BRIDGE-UTTERANCE-EXPORT: 1 つの jti は期限まで 1 回だけ受ける。
// 期限切れの nonce を消してから INSERT OR IGNORE し、1 行入ったときだけ true (1 つの transaction)。
#include <cstdint>
#include <functional>

#include "base/sqlite.hpp"
#include "persona_bridge/authorization_assertion.hpp"

namespace di::persona_bridge {

using AssertionReplayConsumer = std::function<bool(const VerifiedPersonaBridgeAssertion&, std::int64_t now_ms)>;

bool consume_persona_bridge_assertion(sql::Database& db, const VerifiedPersonaBridgeAssertion& assertion, std::int64_t now_ms);

/** flow DB (get_flow_db) の persona_bridge_assertion_nonce を使う既定の consumer。 */
bool consume_default_persona_bridge_assertion(const VerifiedPersonaBridgeAssertion& assertion, std::int64_t now_ms);

} // namespace di::persona_bridge
