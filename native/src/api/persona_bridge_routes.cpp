// @implements SPEC-PERSONA-BRIDGE-UTTERANCE-EXPORT
#include "api/persona_bridge_routes.hpp"

#include <limits>
#include <stdexcept>

#include "base/env.hpp"
#include "base/log.hpp"
#include "base/strings.hpp"
#include "base/time.hpp"
#include "config.hpp"
#include "core/index.hpp"
#include "core/kg_registry.hpp"
#include "persona_bridge/authorization_assertion.hpp"

namespace di::api {

namespace {

constexpr std::int64_t kMaxSafeInteger = 9007199254740991LL;

bool all_digits(std::string_view s) {
    if (s.empty()) return false;
    for (const char c : s)
        if (c < '0' || c > '9') return false;
    return true;
}

/** Number(digits) が安全な整数ならその値。 */
std::optional<std::int64_t> safe_integer(std::string_view digits) {
    std::int64_t v = 0;
    for (const char c : digits) {
        if (v > (kMaxSafeInteger - (c - '0')) / 10) return std::nullopt;
        v = v * 10 + (c - '0');
    }
    return v;
}

std::int64_t parse_since(const std::optional<std::string>& value) {
    if (!value || value->empty()) return 0;
    std::optional<std::int64_t> parsed;
    // /^\d+$/ なら Number、違えば Date.parse (C++ は ISO 8601 だけを読む。port-notes 参照)。
    if (all_digits(*value)) parsed = safe_integer(*value);
    else parsed = time::parse_iso(*value);
    if (!parsed || *parsed < 0 || *parsed > kMaxSafeInteger) throw std::invalid_argument("since must be an ISO date or epoch ms");
    return *parsed;
}

std::int64_t parse_limit(const std::optional<std::string>& value) {
    const std::string raw = value.value_or("500");
    const std::string message = "limit must be an integer in 1.." + std::to_string(kPersonaBridgeMaxLimit);
    if (!all_digits(raw)) throw std::invalid_argument(message);
    const auto parsed = safe_integer(raw);
    if (!parsed || *parsed < 1 || *parsed > kPersonaBridgeMaxLimit) throw std::invalid_argument(message);
    return *parsed;
}

std::vector<persona_bridge::ExportableUtteranceRecord> default_reader(const PersonaBridgeUtteranceQuery& input) {
    core::CoreOptions co;
    co.sqlitePath = core::resolve_active_kg_path(config::get_config());
    auto core = core::create_core(co);
    struct Closer {
        core::Core& c;
        ~Closer() {
            try {
                c.close();
            } catch (...) {
            }
        }
    } closer{*core};
    return persona_bridge::read_human_utterances(core->raw(), input.authorId, input.after, input.limit);
}

const cernere_service_token::PublicKeyLoader& default_key_loader() {
    static const cernere_service_token::PublicKeyLoader loader = cernere_service_token::create_cernere_public_key_loader(
        {[] { return env::get("CERNERE_BASE_URL").value_or(""); }, {}, {}});
    return loader;
}

http::Response json_error(const std::string& message, int status) {
    auto r = http::Response::json(Json{{"ok", false}, {"error", message}}, status);
    r.headers.set("cache-control", "private, no-store");
    return r;
}

} // namespace

http::Router persona_bridge_routes(PersonaBridgeRouteDeps deps) {
    if (!deps.readUtterances) deps.readUtterances = default_reader;
    if (!deps.token) deps.token = [] { return env::get("DISCUTERE_PERSONA_BRIDGE_TOKEN").value_or(""); };
    if (!deps.serviceAudience) deps.serviceAudience = [] { return env::get("CERNERE_PROJECT_STORAGE_SLUG").value_or(""); };
    if (!deps.loadServiceTokenKeys) deps.loadServiceTokenKeys = [] { return default_key_loader()(); };
    if (!deps.assertionPublicKey)
        deps.assertionPublicKey = [] { return env::get("DISCUTERE_PERSONA_BRIDGE_ASSERTION_PUBLIC_KEY").value_or(""); };
    if (!deps.consumeAssertion) deps.consumeAssertion = persona_bridge::consume_default_persona_bridge_assertion;
    if (!deps.now) deps.now = [] { return time::now_ms(); };

    http::Router routes;
    routes.get("/persona-bridge/utterances", [deps](http::Request& req) {
        // 認証集約 P4: Cernere service token と従来の固定トークンの両受理 (bridge_auth)。
        // 本人の生発話を返す経路なので、固定トークンは 32 文字未満を未設定 (503) と扱い default-deny を保つ。
        persona_bridge::BridgeAuthOptions auth_opts;
        auth_opts.token = deps.token;
        auth_opts.serviceAudience = deps.serviceAudience;
        auth_opts.loadServiceTokenKeys = deps.loadServiceTokenKeys;
        auth_opts.now = deps.now;
        auth_opts.label = "persona-bridge";
        const auto auth = persona_bridge::authorize_bridge_request(req.header("authorization"), auth_opts);
        if (!auth.ok) return json_error(auth.error, auth.status);
        const std::string public_key = deps.assertionPublicKey();
        if (str::trim(public_key).empty()) return json_error("persona bridge assertion verification is not configured", 503);

        const std::string author_id = req.query("authorId").value_or("");
        if (!all_digits(author_id) || author_id.size() < 5 || author_id.size() > 24)
            return json_error("authorId must be a Discord snowflake", 400);

        // クエリ検証の失敗だけを 400 で返す。
        PersonaBridgeUtteranceQuery query;
        try {
            const auto since = req.query("since");
            const auto cursor = req.query("cursor");
            if (since && !since->empty() && cursor && !cursor->empty())
                throw std::invalid_argument("use either since or cursor, not both");
            query.authorId = author_id;
            if (cursor && !cursor->empty()) query.after = persona_bridge::decode_utterance_cursor(*cursor);
            else query.after = persona_bridge::UtteranceCursor{parse_since(since), ""};
            query.limit = parse_limit(req.query("limit"));
        } catch (const std::exception& e) {
            return json_error(e.what(), 400);
        }

        const std::int64_t now_ms = deps.now();
        persona_bridge::VerifiedPersonaBridgeAssertion assertion;
        try {
            assertion = persona_bridge::verify_persona_bridge_assertion(req.header(PERSONA_BRIDGE_ASSERTION_HEADER).value_or(""), public_key,
                                                                        author_id, now_ms);
        } catch (const persona_bridge::PersonaBridgeAssertionConfigurationError& e) {
            log::warn(std::string("[persona-bridge] assertion configuration failed: ") + e.what());
            return json_error("persona bridge assertion verification is not configured", 503);
        } catch (const persona_bridge::PersonaBridgeAssertionVerificationError&) {
            return json_error("unauthorized", 401);
        }

        try {
            if (!deps.consumeAssertion(assertion, now_ms)) return json_error("unauthorized", 401);
            // 1 件だけ lookahead し、実際に次の行がある場合に限って cursor を返す。
            PersonaBridgeUtteranceQuery lookahead = query;
            lookahead.limit = query.limit + 1;
            auto records = deps.readUtterances(lookahead);
            const bool has_more = static_cast<std::int64_t>(records.size()) > query.limit;
            if (static_cast<std::int64_t>(records.size()) > query.limit) records.resize(static_cast<std::size_t>(query.limit));
            Json utterances = Json::array();
            for (const auto& u : records) utterances.push_back(Json{{"text", u.text}, {"createdAt", u.createdAt}});
            Json next = nullptr;
            if (has_more) {
                const auto c = persona_bridge::next_utterance_cursor(records);
                next = c ? Json(*c) : Json(nullptr);
            }
            auto r = http::Response::json(Json{{"ok", true}, {"utterances", std::move(utterances)}, {"nextCursor", std::move(next)}});
            r.headers.set("cache-control", "private, no-store");
            return r;
        } catch (...) {
            // DB path や接続先など、内部例外の本文を共有ログへ流さない。
            log::warn("[persona-bridge] utterance export failed");
            return json_error("failed to read utterances", 500);
        }
    });
    return routes;
}

} // namespace di::api
