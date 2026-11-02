#include "flow/user_voices/runtime.hpp"

#include "base/env.hpp"
#include "base/strings.hpp"
#include "cernere_service_token/service_token_client.hpp"
#include "core/vectors/voice_index.hpp"
#include "crawler/sources/attribution_store.hpp"
#include "crawler/sources/steam.hpp"
#include "discatier_engine_adapter/keyword_terms.hpp"
#include "flow/learning_autocrawl.hpp"
#include "flow/paper_review_intent.hpp"
#include "flow/user_voices/game_voices.hpp"
#include "flow/user_voices/mix.hpp"
#include "flow/user_voices/voluptas_impressions.hpp"
#include "flow/voluptas_persona_client.hpp"

namespace di::flow::user_voices {

namespace cs = crawler::sources;

namespace {

bool is_quote_open(std::uint32_t cp) { return cp == 0x300C || cp == 0x300E; }   // 「 『
bool is_quote_close(std::uint32_t cp) { return cp == 0x300D || cp == 0x300F; }  // 」 』

bool starts_with_synthetic(const std::string& s) {
    static const std::string tag = "[synthetic]";
    return s.size() >= tag.size() && str::iequals_ascii(std::string_view(s).substr(0, tag.size()), tag);
}

} // namespace

std::vector<std::string> quoted_terms(const std::string& text) {
    // /[「『]([^」』]{2,40})[」』]/g を線形の走査で (中身は UTF-16 単位で 2〜40)。
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < text.size()) {
        std::size_t next = i;
        const std::uint32_t cp = str::next_codepoint(text, next);
        if (!is_quote_open(cp)) {
            i = next;
            continue;
        }
        std::size_t j = next;
        std::size_t units = 0;
        bool matched = false;
        while (j < text.size()) {
            std::size_t k = j;
            const std::uint32_t c = str::next_codepoint(text, k);
            if (is_quote_close(c)) {
                if (units >= 2) {
                    out.push_back(str::trim(std::string_view(text).substr(next, j - next)));
                    i = k;
                    matched = true;
                }
                break;
            }
            units += c >= 0x10000 ? 2 : 1;
            if (units > 40) break;
            j = k;
        }
        if (!matched) i = next;
    }
    return out;
}

GameRef theme_game_ref(const std::string& theme) {
    GameRef g;
    g.title = theme;
    g.searchTerms = quoted_terms(theme);
    for (auto& t : discatier_engine_adapter::extract_key_terms({theme})) g.searchTerms.push_back(std::move(t));
    g.matchAgainst = theme;
    return g;
}

GameRef similar_game_ref(const std::string& name) { return GameRef{name, {name}, name}; }

std::vector<std::string> parse_similar_games(const std::optional<std::string>& text) {
    if (!text || text->empty()) return {};
    const auto intent = classify_paper_review_intent(*text);
    return intent.kind == "similar_games" ? intent.terms : std::vector<std::string>{};
}

core::vectors::EmbeddingConfig embedding_config_of(const config::DiscutereConfig& cfg) {
    core::vectors::EmbeddingConfig e;
    e.enabled = cfg.embedding.enabled;
    e.baseUrl = cfg.embedding.baseUrl;
    e.model = cfg.embedding.model;
    e.apiKey = cfg.embedding.apiKey;
    e.timeoutMs = cfg.embedding.timeoutMs;
    e.batchSize = cfg.embedding.batchSize;
    return e;
}

CollectUserVoicesDeps default_collect_deps(core::Core* core, const std::string& workspace_id, LogFn log, LogFn warn,
                                           const config::DiscutereConfig* config) {
    const config::DiscutereConfig& cfg = config ? *config : config::get_config();
    const auto& uv = cfg.flow.userVoices;
    std::shared_ptr<core::vectors::EmbeddingClient> embedder;
    if (cfg.embedding.enabled) embedder = core::vectors::create_open_ai_compat_embedder(embedding_config_of(cfg));
    cernere_service_token::ServiceTokenProviderOptions sto;
    sto.targetProjectKey = VOLUPTAS_TARGET_PROJECT_KEY;
    sto.config = [] { return cernere_service_token::service_token_client_config_from_env(); };
    auto service_token = cernere_service_token::create_service_token_provider(std::move(sto));

    CollectUserVoicesDeps d;
    d.core = core;
    d.workspaceId = workspace_id;
    d.resolveSteam = [](const GameRef& game) {
        ResolveSteamAppArgs a;
        a.terms = game.searchTerms;
        a.matchAgainst = game.matchAgainst;
        return resolve_steam_app(a);
    };
    const std::vector<std::string> languages = uv.steamLanguages;
    const double max_reviews = uv.steamMaxReviews;
    d.fetchSteam = [languages, max_reviews](const SteamApp& app, const std::string& game_slug) {
        cs::SteamFetchOptions o;
        o.appId = app.appId;
        o.gameSlug = game_slug;
        o.languages = languages;
        o.maxReviews = static_cast<std::size_t>(max_reviews > 0 ? max_reviews : 0);
        return cs::fetch_steam_reviews(o);
    };
    if (!uv.voluptasBaseUrl.empty()) {
        const std::string base_url = uv.voluptasBaseUrl;
        const int limit = static_cast<int>(uv.glabMaxImpressions);
        d.fetchGlab = [base_url, limit, service_token, warn](const GameRef& game, const std::string& game_slug) {
            ResolveVoluptasBearerArgs b;
            b.serviceToken = service_token;
            b.fallbackToken = env::get("DISCUTERE_VOLUPTAS_EXPORT_TOKEN").value_or("");
            b.log = warn;
            FetchVoluptasArgs a;
            a.baseUrl = base_url;
            a.bearer = resolve_voluptas_bearer(b);
            a.game = game.title;
            a.gameSlug = game_slug;
            a.limit = limit;
            return fetch_voluptas_impressions(a);
        };
    }
    d.importItems = default_import;
    d.storedGlabCount = [](const std::string& game_slug) {
        auto attribution = cs::open_attribution_store();
        const auto n = attribution->list_by_source("glab", game_slug, 2000).size();
        attribution->close();
        return static_cast<int>(n);
    };
    if (embedder) {
        const int batch = static_cast<int>(cfg.embedding.batchSize);
        d.embed = [embedder, workspace_id, batch](core::Core* c, const std::vector<std::string>& ids) {
            if (!c) return 0;
            core::vectors::EmbedVoiceOptions o;
            o.workspaceId = workspace_id;
            o.batchSize = batch;
            return static_cast<int>(core::vectors::embed_voices_by_ids(*c, *embedder, ids, o));
        };
    }
    d.log = std::move(log);
    d.warn = std::move(warn);
    return d;
}

PreparedUserVoices prepare_user_voices(const PrepareUserVoicesArgs& args) {
    const config::DiscutereConfig& cfg = args.config ? *args.config : config::get_config();
    const LogFn log = args.log ? args.log : LogFn([](const std::string&) {});
    const LogFn warn = args.warn ? args.warn : LogFn([](const std::string&) {});
    if (!cfg.flow.userVoices.enabled || !args.openCore) return {};

    const bool include_theme = args.includeThemeGame.value_or(true);
    std::vector<GameRef> games;
    if (include_theme) games.push_back(theme_game_ref(args.theme));
    for (const auto& s : args.similarGames) games.push_back(similar_game_ref(s));
    if (games.empty()) return {};

    PreparedUserVoices out;
    std::optional<std::vector<double>> query_vector;
    {
        auto core = args.openCore();
        struct Closer {
            core::Core* c;
            ~Closer() {
                try {
                    if (c) c->close();
                } catch (...) {
                }
            }
        } closer{core.get()};
        CollectUserVoicesDeps deps =
            args.collectDeps ? args.collectDeps(core.get()) : default_collect_deps(core.get(), args.workspaceId, log, warn, &cfg);
        if (!deps.existingCount) {
            const ListExternalVoicesFn base = args.baseLookup;
            deps.existingCount = [base](const GameRef& game) {
                if (!base) return 0;
                int n = 0;
                for (const auto& v : base({game.title}, 20))
                    if (!starts_with_synthetic(v.content) && v.source != "llm") ++n;
                return n;
            };
        }
        if (args.youtubeApiKey && !args.youtubeApiKey->empty() && !deps.fetchYoutube) {
            const std::string key = *args.youtubeApiKey;
            deps.fetchYoutube = [key](const GameRef& game, const std::string& game_slug) {
                CollectorContext ctx;
                ctx.theme = game.title;
                ctx.gameSlug = game_slug;
                ctx.spec = AutoCrawlSpec{"youtube", game.title, std::nullopt, std::nullopt, std::nullopt};
                ctx.maxItems = 50;
                ctx.youtubeApiKey = key;
                return default_collectors().at("youtube")(ctx);
            };
        }
        out.reports = collect_user_voices(games, deps);
        bool any_slug = false;
        for (const auto& r : out.reports) any_slug = any_slug || r.gameSlug.has_value();
        if (cfg.embedding.enabled && any_slug) {
            try {
                auto embedder = args.embedder ? args.embedder : core::vectors::create_open_ai_compat_embedder(embedding_config_of(cfg));
                auto vecs = embedder->embed({args.theme});
                if (!vecs.empty()) query_vector = std::move(vecs[0]);
            } catch (const std::exception& e) {
                warn(std::string("議題の埋め込みに失敗 (新しい順で使う): ") + e.what());
            }
        }
    }

    bool any_slug = false;
    for (const auto& r : out.reports) any_slug = any_slug || r.gameSlug.has_value();
    if (!any_slug) return out;

    const auto reports = out.reports;
    const auto open_core = args.openCore;
    const auto open_attribution = args.openAttribution;
    const auto base = args.baseLookup;
    const std::string ws = args.workspaceId;
    out.lookup = make_mixed_voice_lookup([reports, open_core, open_attribution, base, ws, include_theme, query_vector, warn](
                                             const std::vector<std::string>& terms, int per_group) {
        auto c = open_core();
        auto attribution = open_attribution ? open_attribution() : cs::open_attribution_store();
        struct Closer {
            core::Core* c;
            cs::AttributionStore* a;
            ~Closer() {
                try {
                    if (a) a->close();
                } catch (...) {
                }
                try {
                    if (c) c->close();
                } catch (...) {
                }
            }
        } closer{c.get(), attribution.get()};
        try {
            std::vector<std::vector<ContextVoice>> groups;
            for (std::size_t i = 0; i < reports.size(); ++i) {
                const auto& r = reports[i];
                if (r.channel == "di") {
                    groups.push_back(base ? base({r.title}, per_group) : std::vector<ContextVoice>{});
                } else if (r.gameSlug) {
                    ListGameVoicesArgs a;
                    a.core = c.get();
                    a.attribution = attribution.get();
                    a.workspaceId = ws;
                    a.gameSlug = *r.gameSlug;
                    a.limit = per_group;
                    a.queryVector = query_vector;
                    groups.push_back(list_game_voices(a));
                } else if (include_theme && i == 0 && base) {
                    // 議題のゲームの声が集まらなければ、議題側は既存のキーワード検索で埋める。
                    groups.push_back(base(terms, per_group));
                } else {
                    groups.emplace_back();
                }
            }
            return groups;
        } catch (const std::exception& e) {
            warn(std::string("ユーザーの声の引き出しに失敗 (既存の検索で継続): ") + e.what());
            return std::vector<std::vector<ContextVoice>>{base ? base(terms, per_group) : std::vector<ContextVoice>{}};
        }
    });
    return out;
}

} // namespace di::flow::user_voices
