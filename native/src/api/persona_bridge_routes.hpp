#pragma once
// @implements SPEC-PERSONA-BRIDGE-UTTERANCE-EXPORT
// src/api/persona-bridge-routes.ts の移植: GET /persona-bridge/utterances ("/api" に mount する前提)。
// 順序と status は TS と同じ:
//   1. Cache-Control: private, no-store
//   2. Authorization (Cernere service token か 32 文字以上の固定トークン、persona_bridge::authorize_bridge_request) → 401/403/503
//   3. アサーション公開鍵が未設定なら 503
//   4. authorId が snowflake (/^[0-9]{5,24}$/) でなければ 400
//   5. since / cursor / limit の検証 (400)
//   6. 本人確認アサーション (Ed25519、1 回限り) → 設定不備 503 / 不正 401 / 再送 401
//   7. limit+1 件を読み、次があるときだけ nextCursor を返す。読み出しの失敗は 500 + 定型文 (内部の詳細を出さない)。
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "cernere_service_token/cernere_public_keys.hpp"
#include "infra/http.hpp"
#include "persona_bridge/assertion_replay_store.hpp"
#include "persona_bridge/bridge_auth.hpp"
#include "persona_bridge/utterance_export.hpp"

namespace di::api {

inline constexpr std::int64_t kPersonaBridgeMaxLimit = 1'000;
inline constexpr const char* PERSONA_BRIDGE_ASSERTION_HEADER = "x-discutere-persona-assertion";

struct PersonaBridgeUtteranceQuery {
    std::string authorId;
    persona_bridge::UtteranceCursor after;
    std::int64_t limit = 0;
};

using PersonaBridgeUtteranceReader =
    std::function<std::vector<persona_bridge::ExportableUtteranceRecord>(const PersonaBridgeUtteranceQuery&)>;

/** 省略したものは既定 (env / Cernere 公開鍵 / flow DB の nonce 表 / active KG) を使う。 */
struct PersonaBridgeRouteDeps {
    PersonaBridgeUtteranceReader readUtterances;
    std::function<std::string()> token;
    std::function<std::string()> serviceAudience;
    cernere_service_token::PublicKeyLoader loadServiceTokenKeys;
    std::function<std::string()> assertionPublicKey;
    persona_bridge::AssertionReplayConsumer consumeAssertion;
    std::function<std::int64_t()> now;
};

http::Router persona_bridge_routes(PersonaBridgeRouteDeps deps = {});

} // namespace di::api
