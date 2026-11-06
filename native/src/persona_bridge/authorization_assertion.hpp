#pragma once
// persona bridge の本人確認アサーション (Ed25519 署名つき、短命・1 回限り)。
// 移植元 src/persona-bridge/authorization-assertion.ts (SPEC-PERSONA-BRIDGE-UTTERANCE-EXPORT)。
// 形式: base64url(payload JSON) "." base64url(Ed25519 署名 (payload 部の文字列に対する))。
#include <cstdint>
#include <stdexcept>
#include <string>

namespace di::persona_bridge {

inline constexpr const char* PERSONA_BRIDGE_AUDIENCE = "discutere-persona-bridge";
inline constexpr std::int64_t MAX_ASSERTION_TTL_SECONDS = 5 * 60;

struct VerifiedPersonaBridgeAssertion {
    std::string authorId;
    std::int64_t expiresAt = 0;
    std::string jti;
};

/** 公開鍵の設定が無い・不正 (運用側の誤り)。 */
class PersonaBridgeAssertionConfigurationError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};
/** アサーションが不正 (要求側の誤り。401 で拒否する)。 */
class PersonaBridgeAssertionVerificationError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/**
 * 検証する。拒否条件と順序は TS と同じ: 形式 → 大きさ (署名前) → 署名 → payload → authorId (snowflake・一致)
 * → aud → exp (整数) → jti → 期限 (now < exp <= now + 5 分)。
 * public_key は Ed25519 の SPKI DER を base64url にしたもの。
 */
VerifiedPersonaBridgeAssertion verify_persona_bridge_assertion(const std::string& compact, const std::string& public_key,
                                                               const std::string& author_id, std::int64_t now_ms);

/** Buffer.from(s, "base64url") 相当 (Node と同じく寛容)。 */
std::string node_base64url_decode(const std::string& s);

} // namespace di::persona_bridge
