#include "persona_bridge/assertion_replay_store.hpp"

#include "flow/db/connection.hpp"

namespace di::persona_bridge {

bool consume_persona_bridge_assertion(sql::Database& db, const VerifiedPersonaBridgeAssertion& assertion, std::int64_t now_ms) {
    // Math.floor(nowMs / 1000) (負の時刻も floor)
    const std::int64_t now_seconds = now_ms >= 0 ? now_ms / 1000 : -((-now_ms + 999) / 1000);
    return db.transaction([&] {
        db.run("DELETE FROM persona_bridge_assertion_nonce WHERE expires_at <= ?", {now_seconds});
        return db.run("INSERT OR IGNORE INTO persona_bridge_assertion_nonce (jti, expires_at) VALUES (?, ?)", {assertion.jti, assertion.expiresAt})
                   .changes == 1;
    });
}

bool consume_default_persona_bridge_assertion(const VerifiedPersonaBridgeAssertion& assertion, std::int64_t now_ms) {
    return consume_persona_bridge_assertion(*flow::db::get_flow_db(), assertion, now_ms);
}

} // namespace di::persona_bridge
