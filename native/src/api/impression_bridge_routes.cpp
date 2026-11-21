#include "api/impression_bridge_routes.hpp"

#include <memory>

#include "base/env.hpp"
#include "base/log.hpp"
#include "base/time.hpp"
#include "config.hpp"
#include "core/index.hpp"
#include "core/kg_registry.hpp"
#include "core/vectors/voice_index.hpp"
#include "flow/db/connection.hpp"
#include "flow/learning_autocrawl.hpp"
#include "flow/user_voices/runtime.hpp"
#include "persona_bridge/bridge_auth.hpp"
#include "playtest/voice_store.hpp"

namespace di::api {

void default_impression_ingest(const std::function<void(const persona_bridge::IngestDeps&)>& run) {
    const auto& cfg = config::get_config();
    auto core = core::create_core(core::CoreOptions{core::resolve_active_kg_path(cfg), std::nullopt});
    struct Closer {
        core::Core* c;
        ~Closer() {
            try {
                c->close();
            } catch (...) {
            }
        }
    } closer{core.get()};
    std::shared_ptr<core::vectors::EmbeddingClient> embedder;
    if (cfg.embedding.enabled) embedder = core::vectors::create_open_ai_compat_embedder(flow::user_voices::embedding_config_of(cfg));
    persona_bridge::IngestDeps deps;
    core::Core* c = core.get();
    const std::string ws = cfg.workspace;
    deps.importItems = [c, ws](const std::vector<crawler::sources::ExternalUtterance>& items) {
        const int imported = flow::default_import(c, items, ws);
        std::vector<playtest::HumanVoice> voices;
        voices.reserve(items.size());
        for (const auto& item : items) voices.push_back({item.nativeId, item.threadKey, item.content});
        playtest::index_human_playtest_voices(*flow::db::get_flow_db(), voices);
        return imported;
    };
    if (embedder) {
        const int batch = static_cast<int>(cfg.embedding.batchSize);
        deps.embed = [c, embedder, ws, batch](const std::vector<std::string>& ids) {
            core::vectors::EmbedVoiceOptions o;
            o.workspaceId = ws;
            o.batchSize = batch;
            return static_cast<int>(core::vectors::embed_voices_by_ids(*c, *embedder, ids, o));
        };
    }
    deps.warn = [](const std::string& m) { log::warn("[impression-bridge] " + m); };
    run(deps);
}

namespace {

http::Response no_store(http::Response r) {
    r.headers.set("cache-control", "private, no-store");
    return r;
}

} // namespace

http::Router create_impression_bridge_routes(ImpressionBridgeRoutesOptions options) {
    if (!options.token) options.token = [] { return env::get("DISCUTERE_PERSONA_BRIDGE_TOKEN").value_or(""); };
    if (!options.serviceAudience) options.serviceAudience = [] { return env::get("CERNERE_PROJECT_STORAGE_SLUG").value_or(""); };
    if (!options.loadServiceTokenKeys) {
        static const cernere_service_token::PublicKeyLoader default_loader = cernere_service_token::create_cernere_public_key_loader(
            cernere_service_token::CernerePublicKeyLoaderOptions{[] { return env::get("CERNERE_BASE_URL").value_or(""); }, {}, {}});
        options.loadServiceTokenKeys = default_loader;
    }
    if (!options.now) options.now = [] { return time::now_ms(); };
    if (!options.withIngest) options.withIngest = default_impression_ingest;
    if (!options.saveAiReview) options.saveAiReview = [](const playtest::AiReview& review) {
        return playtest::save_ai_review(*flow::db::get_flow_db(), review);
    };
    auto opts = std::make_shared<ImpressionBridgeRoutesOptions>(std::move(options));

    http::Router routes;
    routes.post("/persona-bridge/ai-playtest-reviews", [opts](http::Request& req) {
        const auto auth = persona_bridge::authorize_bridge_request(
            req.header("authorization"), {opts->token, opts->serviceAudience, opts->loadServiceTokenKeys, opts->now, "ai-playtest-bridge"});
        if (!auth.ok) return no_store(http::Response::json(Json{{"ok", false}, {"error", auth.error}}, auth.status));
        const auto body = req.json(JsonLimits{playtest::review_body_limit, 6'000, 8});
        const auto key = req.header("idempotency-key");
        const auto review = body && key ? playtest::parse_ai_review(*body, *key) : std::nullopt;
        if (!review) return no_store(http::Response::json(Json{{"ok", false}, {"error", "invalid AI playtest review"}}, 400));
        try {
            const bool inserted = opts->saveAiReview(*review);
            return no_store(http::Response::json(Json{{"ok", true}, {"run_id", review->runId}}, inserted ? 201 : 200));
        } catch (const std::exception&) {
            log::warn("[ai-playtest-bridge] failed to store review");
            return no_store(http::Response::json(Json{{"ok", false}, {"error", "failed to store review"}}, 500));
        }
    }, http::RouteOptions{playtest::review_body_limit});
    routes.post(
        "/persona-bridge/impressions",
        [opts](http::Request& req) {
            const auto auth = persona_bridge::authorize_bridge_request(
                req.header("authorization"),
                persona_bridge::BridgeAuthOptions{opts->token, opts->serviceAudience, opts->loadServiceTokenKeys, opts->now, "impression-bridge"});
            if (!auth.ok) return no_store(http::Response::json(Json{{"ok", false}, {"error", auth.error}}, auth.status));

            persona_bridge::ParsedImpressionBatch parsed;
            const auto body = req.json(persona_bridge::kImpressionBodyJsonLimits);
            if (!body) return no_store(http::Response::json(Json{{"ok", false}, {"error", "body must be JSON"}}, 400));
            try {
                parsed = persona_bridge::parse_impression_batch(*body);
            } catch (const persona_bridge::ImpressionBatchError& e) {
                return no_store(http::Response::json(Json{{"ok", false}, {"error", e.what()}}, 400));
            }
            try {
                std::optional<persona_bridge::IngestResult> result;
                opts->withIngest([&](const persona_bridge::IngestDeps& deps) { result = persona_bridge::ingest_impressions(parsed.impressions, deps); });
                const persona_bridge::IngestResult r = result.value_or(persona_bridge::IngestResult{});
                if (parsed.skipped.non_anonymous_field > 0) {
                    log::warn("[impression-bridge] 匿名でない項目を含む感想 " + std::to_string(parsed.skipped.non_anonymous_field) + " 件を拒否");
                }
                Json out{{"ok", true}};
                const Json rj = persona_bridge::to_json(r);
                for (auto it = rj.begin(); it != rj.end(); ++it) out[it.key()] = it.value();
                out["skipped"] = persona_bridge::to_json(parsed.skipped);
                return no_store(http::Response::json(out));
            } catch (...) {
                // KG パスなど内部例外の本文は応答にもログにも出さない。
                log::warn("[impression-bridge] impression ingest failed");
                return no_store(http::Response::json(Json{{"ok", false}, {"error", "failed to ingest impressions"}}, 500));
            }
        },
        http::RouteOptions{persona_bridge::kImpressionBodyMaxBytes});
    return routes;
}

} // namespace di::api
