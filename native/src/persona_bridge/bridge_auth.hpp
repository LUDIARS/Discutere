#pragma once
// Voluptas → Discutere ブリッジの呼び出し認証 (認証集約 P4, src/persona-bridge/bridge-auth.ts)。
//
// 同じ `Authorization: Bearer` で次のどちらかを受ける:
//   - Cernere service token (PASETO v4.public。aud = Discutere の storage_slug、scope = persona-bridge:write)
//     → Cernere 公開鍵で検証する。Discutere 自身は Cernere の credential を持たなくてよい (検証だけ)。
//   - 従来の固定トークン (DISCUTERE_PERSONA_BRIDGE_TOKEN、32 文字以上。P5 で撤去予定)。比較は定数時間。
// 固定トークンが短い/未設定なら 503 (default-deny)。理由の詳細は応答に出さない。token はログに出さない。
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "cernere_service_token/cernere_public_keys.hpp"

namespace di::persona_bridge {

/** 推測可能な短い token を「設定済み」と見なさない。 */
inline constexpr std::size_t MIN_BRIDGE_TOKEN_LENGTH = 32;
/** Cernere service token でブリッジを呼ぶのに要る scope。 */
inline constexpr const char* PERSONA_BRIDGE_SERVICE_SCOPE = "persona-bridge:write";

struct BridgeAuthOptions {
    /** 固定トークン (DISCUTERE_PERSONA_BRIDGE_TOKEN)。 */
    std::function<std::string()> token;
    /** service token の aud (= CERNERE_PROJECT_STORAGE_SLUG)。 */
    std::function<std::string()> serviceAudience;
    cernere_service_token::PublicKeyLoader loadServiceTokenKeys;
    std::function<std::int64_t()> now;
    /** ログの接頭辞 (例 "persona-bridge")。 */
    std::string label;
};

struct BridgeAuthResult {
    bool ok = false;
    int status = 0;  // 401 | 403 | 503
    std::string error;
};

/** `Bearer <空白を含まない 1 語>` だけを受ける (/^Bearer ([^\s]+)$/)。それ以外は空文字。 */
std::string bearer_token(const std::optional<std::string>& header);

/** Authorization ヘッダを検証する。 */
BridgeAuthResult authorize_bridge_request(const std::optional<std::string>& header, const BridgeAuthOptions& opts);

} // namespace di::persona_bridge
