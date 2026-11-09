// discutere-server のエントリ (src/index.ts の移植)。
//
// index.ts と同じ順序・同じ条件でサーバを組み立てる:
//   DB 初期化 → console capture → Vestigium → config → startup guard → Router (CORS / health / 各ルートの mount)
//   → kg-sync scheduler → LLM と persona engine / facilitator / consensus scorer / auto seed → economy → web-chat
//   → flow routes / spec extract → external discussion → backup scheduler → cost relay → Discord Gateway → Slack → listen。
// ルートの mount 順は Hono の登録順と同じ (userContext の `/api/*` の位置が意味を持つ)。
//
// TS との差 (native/docs/port-notes/w6-main.md):
//   - AnthropicSdkClient (API キー / OAuth で api.anthropic.com を直接呼ぶ経路) は持たない。TS が AnthropicSdkClient を
//     作っていた条件 (backend が worker-pool / local / claude-cli 以外で anthropicApiKey あり) では warn して
//     ClaudeCliClient (claude -p) にする。worker-pool 時の flow LLM の鎖からも SDK を外す (worker-pool → claude -p)。
//   - Discord Gateway の起動は専用スレッドで行い、HTTP の listen を待たせない (TS の startDiscordGateway は Promise)。
//   - 終了 (SIGINT / SIGTERM) は TS の worker pool / kg sync / slack / vestigium に加え、HTTP・Gateway・各タイマー・executor を止める。
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "api/admin_routes.hpp"
#include "api/blackbox_routes.hpp"
#include "api/conclusion_publication_routes.hpp"
#include "api/consensus_routes.hpp"
#include "api/dashboard_routes.hpp"
#include "api/datasource_routes.hpp"
#include "api/economy_graph_routes.hpp"
#include "api/errors_routes.hpp"
#include "api/external_discussion_routes.hpp"
#include "api/glab_review_trend_routes.hpp"
#include "api/guide_routes.hpp"
#include "api/impression_bridge_routes.hpp"
#include "api/kg_migration_routes.hpp"
#include "api/learning_view_routes.hpp"
#include "api/noise_routes.hpp"
#include "api/persona_bridge_routes.hpp"
#include "api/persona_routes.hpp"
#include "api/queue_routes.hpp"
#include "api/top_page_routes.hpp"
#include "api/tuning_routes.hpp"
#include "api/web_chat_routes.hpp"
#include "api/worker.hpp"
#include "api/worker_pool_control.hpp"
#include "backup/runner.hpp"
#include "base/env.hpp"
#include "base/executor.hpp"
#include "base/fsutil.hpp"
#include "base/json.hpp"
#include "base/log.hpp"
#include "base/sqlite.hpp"
#include "base/strings.hpp"
#include "config.hpp"
#include "core/index.hpp"
#include "core/kg_registry.hpp"
#include "crawler/sources/summarize.hpp"
#include "db/connection.hpp"
#include "discatier_engine_adapter/event_bridge.hpp"
#include "discatier_engine_adapter/index.hpp"
#include "discord_hook/auto_discussion.hpp"
#include "discord_hook/discussion_bridge.hpp"
#include "discord_hook/gateway.hpp"
#include "discord_hook/reactions.hpp"
#include "discussion_seed/scheduler.hpp"
#include "external_discussion/service.hpp"
#include "external_discussion/store.hpp"
#include "feedback/store.hpp"
#include "flow/cost_relay.hpp"
#include "flow/db/connection.hpp"
#include "flow/entry_discord.hpp"
#include "flow/external_voices.hpp"
#include "flow/learning_autocrawl.hpp"
#include "flow/llm_fallback.hpp"
#include "flow/model_router.hpp"
#include "flow/persona_adopt_runner.hpp"
#include "flow/web/routes.hpp"
#include "flow/web/spec_extract_route.hpp"
#include "infra/http.hpp"
#include "infra/http_server.hpp"
#include "kg_sync/scheduler.hpp"
#include "lib/sentiment_core/cascade.hpp"
#include "ludus/economy_analyzer.hpp"
#include "middleware/auth.hpp"
#include "observability/console_capture.hpp"
#include "persona_engine/facilitator/facilitator.hpp"
#include "persona_engine/index.hpp"
#include "persona_engine/llm/codex_cli.hpp"
#include "persona_engine/scoring/consensus_scorer.hpp"
#include "persona_engine/worker_pool/client.hpp"
#include "persona_engine/worker_pool/debate_rules.hpp"
#include "persona_engine/worker_pool/persona_prompts.hpp"
#include "persona_engine/worker_pool/pool.hpp"
#include "queue/snapshot.hpp"
#include "runtime_settings/store.hpp"
#include "secrets/youtube_env.hpp"
#include "slack/slack_router.hpp"
#include "slack/socket_mode.hpp"
#include "startup_guard.hpp"

namespace {

using namespace di;
using persona_engine::LLMClientPtr;

// ─── 小さな補助 ─────────────────────────────────────────────────────────────

std::string what_of(const std::exception& e) { return e.what(); }

/** TS の `x || undefined` (空文字は未指定)。 */
std::optional<std::string> non_empty(const std::optional<std::string>& v) {
    if (v && !v->empty()) return v;
    return std::nullopt;
}

/** config の double (ms) を long long へ (負・非有限は 0)。 */
long long to_ms(double v) {
    if (!(v > 0)) return 0;
    if (v > 9.0e15) return 9'000'000'000'000'000LL;
    return static_cast<long long>(v);
}

/** console.log(`  [tag] ${msg}`, meta) の代わり (meta は JSON で付ける。util.inspect の書式ではない)。 */
class PrefixLogger : public persona_engine::Logger {
public:
    explicit PrefixLogger(std::string tag) : tag_(std::move(tag)) {}
    void debug(const Json&, std::string_view) override {}
    void info(const Json& meta, std::string_view m) override { log::info(line(meta, m)); }
    void warn(const Json& meta, std::string_view m) override { log::warn(line(meta, m)); }
    void error(const Json& meta, std::string_view m) override { log::error(line(meta, m)); }

private:
    std::string line(const Json& meta, std::string_view m) const {
        std::string s = "  [" + tag_ + "] " + std::string(m);
        if (!meta.is_null() && !(meta.is_object() && meta.empty())) s += " " + json_stringify(meta);
        return s;
    }
    std::string tag_;
};

/** LLMClient を sentiment-core の SentimentLlmClient として使う (TS は構造的型付けでそのまま渡していた)。 */
class LlmSentimentClient : public lib::sentiment_core::SentimentLlmClient {
public:
    explicit LlmSentimentClient(LLMClientPtr llm) : llm_(std::move(llm)) {}
    lib::sentiment_core::SentimentLlmResponse invoke(const lib::sentiment_core::SentimentLlmRequest& r) override {
        persona_engine::LLMInvokeArgs a;
        a.prompt = r.prompt;
        a.system = r.system;
        a.maxTokens = r.maxTokens;
        auto res = llm_->invoke(a);
        lib::sentiment_core::SentimentLlmResponse out;
        out.ok = res.ok;
        out.text = res.text;
        out.error = res.error;
        return out;
    }

private:
    LLMClientPtr llm_;
};

crawler::sentiment::CascadeClients sentiment_clients_of(const LLMClientPtr& llm) {
    crawler::sentiment::CascadeClients c;
    c.main = std::make_shared<LlmSentimentClient>(llm);
    return c;
}

/** TS の Record (挿入順) を std::map から TS の順に並べ直す (順に無いキーは後ろに辞書順)。 */
std::vector<std::pair<std::string, std::string>> ordered_pairs(const std::map<std::string, std::string>& m,
                                                              const std::vector<std::string>& order) {
    std::vector<std::pair<std::string, std::string>> out;
    std::set<std::string> seen;
    for (const auto& k : order) {
        auto it = m.find(k);
        if (it != m.end() && seen.insert(k).second) out.emplace_back(it->first, it->second);
    }
    for (const auto& [k, v] : m)
        if (!seen.count(k)) out.emplace_back(k, v);
    return out;
}

/** createCore(resolveActiveKgPath(config))。 */
std::unique_ptr<core::Core> open_active_kg(const config::DiscutereConfig& config) {
    core::CoreOptions o;
    o.sqlitePath = core::resolve_active_kg_path(config);
    return core::create_core(o);
}

// ─── 終了シグナル (ハンドラは async-signal-safe な代入だけ) ─────────────────────
volatile std::sig_atomic_t g_signal = 0;
extern "C" void on_signal(int sig) { g_signal = sig; }

/** gateway 起動後に結線するもの (TS の module-level let を mutex で守る)。 */
struct GatewaySlot {
    std::mutex mu;
    std::shared_ptr<discord_hook::DiscordGateway> gateway;
    bool forum_finalizer = false;  // discord.forum.enabled のときだけ収束 finalizer を結ぶ
    std::shared_ptr<discord_hook::DiscordGateway> get() {
        std::lock_guard lock(mu);
        return gateway;
    }
};

/** persona-engine の IIFE が返すもの。 */
struct PersonaLifecycle {
    std::shared_ptr<persona_engine::PersonaEngineHandle> engine;
    std::shared_ptr<discatier_engine_adapter::EventBridge> bridge;
    std::shared_ptr<core::Core> core;
    sql::DatabasePtr peDb;
    std::shared_ptr<persona_engine::DiscussionContextProvider> adapter;
    std::shared_ptr<persona_engine::facilitator::Facilitator> facilitator;
    std::shared_ptr<discussion_seed::AutoSeedScheduler> autoSeed;
    std::shared_ptr<persona_engine::scoring::ConsensusScorer> consensusScorer;
};

std::string signal_name(int sig) {
    if (sig == SIGINT) return "SIGINT";
    if (sig == SIGTERM) return "SIGTERM";
    return "signal " + std::to_string(sig);
}

int run_server() {
    // Initialize DB (triggers schema creation) — TS は import "./db/connection.js" で最初に開く。
    db::get_main_db();

    // FEATURE ②: console.error/warn を error-buffer に取り込む (admin dashboard 可視化用)。
    // なるべく早く仕掛けて以降の全 console 出力を拾う。
    observability::install_console_capture();

    // LUDIARS 横断ロギング (Concordia file-tail → error-detector で拾われる JSONL 出力)。
    // captureConsole 相当は di::log の sink (Vestigium のコンストラクタが登録する)。
    log::VestigiumOptions vopts;
    vopts.service_code = "discutere";
    auto vestigium = std::make_unique<log::Vestigium>(vopts);

    const config::DiscutereConfig& config = config::get_config();
    // production 起動の前提 (botToken 必須等) を fail-fast 検証する (dev/test は no-op)。
    try {
        assert_startup_health(config);
    } catch (const StartupHealthError& e) {
        log::error(std::string("[discutere] startup guard: ") + e.what());
        vestigium->shutdown();
        return 1;
    }
    auto app = std::make_shared<http::Router>();

    const std::string frontend_url = config.server.frontendUrl;
    app->use("*", http::cors(frontend_url, true, {"GET", "POST", "PUT", "DELETE", "OPTIONS"},
                             {"Content-Type", "Authorization", "X-User-Id", "X-User-Role"}));

    // Health check (認証不要)
    app->get("/health", [](http::Request&) { return http::Response::json(Json{{"status", "ok"}, {"service", "discutere"}}); });

    // ─── トップページ (各 Web UI への入口、 認証不要・loopback) ──────────
    app->mount("/", api::top_page_routes());
    // 使い方ガイド (Discord フォーラム / チャンネルの使い方とルール)。実 config を注入。
    app->mount("/", api::create_guide_routes(config.discord));
    app->mount("/", api::learning_view_routes());
    // 常駐ワーカー callback (内部用)。userContext より前に mount するが、worker 側で loopback ガード (非 loopback は 403)。
    app->mount("/internal", api::worker_routes());

    // ─── 認証ミドルウェア (X-User-Id / X-User-Role ヘッダーを context に載せる) ──
    // Cernere / 独自 JWT 認証層は Discord-only pivot で撤去。実認可は Discord Gateway (bot token + admin-id allowlist) 側。
    app->use("/api/*", middleware::user_context());

    // GLAB には Di が取得済みの Steam レビューをゲーム別に集約した結果だけを渡す。
    app->mount("/api", api::glab_review_trend_routes());
    app->mount("/api", api::create_conclusion_publication_routes());
    // ─── Admin (人間 → AI 介入経路) / dashboard / エラー / 合意スコア / 議論キュー / ノイズ / データソース ──
    app->mount("/api", api::admin_routes());
    app->mount("/api", api::dashboard_routes());
    app->mount("/api", api::errors_routes());
    app->mount("/api", api::consensus_routes());
    app->mount("/api", api::queue_routes());
    app->mount("/api", api::noise_routes());
    app->mount("/api", api::datasource_routes());
    // ─── 常駐ワーカー制御 UI/API / 議論チューニング / ペルソナ / persona-bridge / 感想の受け口 ──
    app->mount("/api", api::worker_pool_control_routes());
    app->mount("/api", api::tuning_routes());
    app->mount("/api", api::persona_routes());
    app->mount("/api", api::persona_bridge_routes());
    // Voluptas が自動で届ける匿名の感想 (spec/feature/flow/voluptas-impression-push.md)。
    app->mount("/api", api::create_impression_bridge_routes());
    // ─── 成長型ブラックボックス レビュー & 管理 ──
    app->mount("/api", api::blackbox_routes());
    // ─── KG 共有同期: 配信エンドポイント (master) /api/kg/migrations ──────
    app->mount("/api", api::kg_migration_routes());
    // follower 側の自動 pull (config.kgSync.auto + sourceUrl のときのみ)。
    std::function<void()> stop_kg_sync = kg_sync::start_kg_sync_scheduler(config);

    LLMClientPtr auto_discussion_llm;
    // 投稿→議題化の分類器専用 LLM (persona engine の worker-pool 経路とは独立)。
    LLMClientPtr classifier_llm;
    std::shared_ptr<persona_engine::worker_pool::WorkerPool> worker_pool;
    std::optional<std::vector<persona_engine::PersonaSeed>> worker_persona_seeds;
    const bool is_worker_pool = config.llm.backend == "worker-pool";
    const std::optional<std::string> git_bash = config.workerPool.gitBashPath ? config.workerPool.gitBashPath : config.llm.gitBashPath;

    // 分類器 (classifyDiscordMessage) 専用 LLM client を config.classifier に従って構築する。
    auto build_classifier_llm = [&]() -> LLMClientPtr {
        const auto& c = config.classifier;
        if (c.backend == "off") {
            log::info("  classifier LLM: off (regex fallback のみ)");
            return nullptr;
        }
        if (c.backend == "anthropic") {
            if (!config.llm.anthropicApiKey || config.llm.anthropicApiKey->empty()) {
                log::info("  classifier LLM: anthropic 指定だが ANTHROPIC_API_KEY 無し → regex fallback");
                return nullptr;
            }
            // C++ 版は API キーで直接呼ぶ経路を持たない → claude -p (Lictor 経由 spawn) にする。
            log::warn("  classifier LLM: anthropic (API 直) は C++ 版では使えないため ClaudeCliClient (claude -p) を使う (model=" + c.model + ")");
            persona_engine::ClaudeCliClientOptions o;
            o.defaultModel = c.model;
            o.defaultTimeoutMs = to_ms(c.timeoutMs);
            o.gitBashPath = git_bash;
            return std::make_shared<persona_engine::ClaudeCliClient>(o);
        }
        if (c.backend == "local") {
            // 分類は llm.local エンドポイントを共用 (model だけ classifier.model で上書き)。
            log::info("  classifier LLM: LocalOpenAiClient (" + config.llm.local.baseUrl + " / model=" + c.model + ")");
            persona_engine::LocalOpenAiClientOptions o;
            o.baseUrl = config.llm.local.baseUrl;
            o.defaultModel = c.model.empty() ? config.llm.local.model : c.model;
            o.apiKey = config.llm.local.apiKey;
            o.defaultTimeoutMs = to_ms(c.timeoutMs);
            return std::make_shared<persona_engine::LocalOpenAiClient>(o);
        }
        // claude-cli = Lictor 経由 spawn。
        log::info("  classifier LLM: ClaudeCliClient / Lictor 経由 spawn (model=" + c.model + ")");
        persona_engine::ClaudeCliClientOptions o;
        o.defaultModel = c.model;
        o.defaultTimeoutMs = to_ms(c.timeoutMs);
        o.gitBashPath = git_bash;
        return std::make_shared<persona_engine::ClaudeCliClient>(o);
    };

    // フォーラム集約の収束 finalizer / 合意スコアラーの 👍 は gateway 起動後に結線する (late-bound)。
    auto gateway_slot = std::make_shared<GatewaySlot>();

    // runtime 設定ストア (収束トリガー / 役割プロンプト / debate instructions の override を SQLite 永続)。
    std::shared_ptr<runtime_settings::RuntimeSettingsStore> runtime_settings;

    // persona-engine 起動 wiring (TS の IIFE)。
    auto persona_lifecycle = [&]() -> std::optional<PersonaLifecycle> {
        LLMClientPtr llm;
        fs::ensure_dir("./data");
        auto settings_db = sql::Database::open("./data/discutere-settings.db");
        runtime_settings::RuntimeSettingsDefaults defaults;
        defaults.facilitator.tickMs = config.facilitator.tickMs;
        defaults.facilitator.idleGapMs = config.facilitator.idleGapMs;
        defaults.facilitator.maxPersonas = config.facilitator.maxPersonas;
        defaults.facilitator.aufhebungTarget = config.facilitator.aufhebungTarget;
        defaults.facilitator.convergePolicy = "default";
        defaults.rolePrompts = ordered_pairs(persona_engine::worker_pool::ROLE_GUIDANCE_DEFAULTS(), {"ファシリテーター", "正論派", "否定派", "意見屋"});
        {
            std::vector<std::string> rule_order;
            for (const auto& r : persona_engine::worker_pool::DEBATE_RULE_SEEDS()) rule_order.push_back(r.id);
            defaults.ruleInstructions = ordered_pairs(persona_engine::worker_pool::RULE_INSTRUCTION_DEFAULTS(), rule_order);
        }
        defaults.costRelay.enabled = config.cost.relay.enabled;
        defaults.costRelay.anatomiaUrl = config.cost.relay.anatomiaUrl.value_or("");
        defaults.costRelay.concordiaUrl = config.cost.relay.concordiaUrl.value_or("");
        defaults.costRelay.service = config.cost.relay.service;
        runtime_settings = runtime_settings::create_runtime_settings_store(settings_db, std::move(defaults));
        {
            std::weak_ptr<runtime_settings::RuntimeSettingsStore> weak = runtime_settings;
            persona_engine::worker_pool::set_role_prompt_resolver([weak](const std::string& role) -> std::optional<std::string> {
                if (auto s = weak.lock()) return s->get_role_prompt(role);
                return std::nullopt;
            });
        }
        api::set_runtime_settings(runtime_settings);

        if (is_worker_pool) {
            std::vector<persona_engine::worker_pool::WorkerConfig> all_workers;
            if (!config.workerPool.workers.empty()) {
                for (const auto& w : config.workerPool.workers) all_workers.push_back({w.id, w.role, w.provider, w.model});
            } else {
                all_workers = persona_engine::worker_pool::DEFAULT_WORKERS();
            }
            // 議論パーティ設定: excludeProviders (例 ["codex"]) の provider を外す。
            std::set<std::string> excluded(config.workerPool.excludeProviders.begin(), config.workerPool.excludeProviders.end());
            std::vector<persona_engine::worker_pool::WorkerConfig> workers;
            for (const auto& w : all_workers)
                if (!excluded.count(w.provider)) workers.push_back(w);
            if (!excluded.empty()) {
                std::vector<std::string> ex(excluded.begin(), excluded.end());
                // TS は Set の挿入順。表示だけなので config の順で出す。
                ex.clear();
                std::set<std::string> seen;
                for (const auto& p : config.workerPool.excludeProviders)
                    if (seen.insert(p).second) ex.push_back(p);
                log::info("  worker-pool: excludeProviders=[" + str::join(ex, ",") + "] → 参加 " + std::to_string(workers.size()) + "/" +
                          std::to_string(all_workers.size()) + " 名");
            }
            persona_engine::worker_pool::WorkerPoolConfig wc;
            wc.enabled = config.workerPool.enabled;
            wc.workspace = config.workerPool.workspace;
            wc.callbackBaseUrl = config.workerPool.callbackBaseUrl;
            // ワーカー (lictor claude/codex) の cwd = 専用 worker-home。
            wc.workerCwd = fs::join(env::cwd(), "worker-home");
            wc.gitBashPath = config.workerPool.gitBashPath;
            wc.injectDelayMs = to_ms(config.workerPool.injectDelayMs);
            wc.turnTimeoutMs = to_ms(config.workerPool.turnTimeoutMs);
            wc.registerTimeoutMs = to_ms(config.workerPool.registerTimeoutMs);
            wc.registerSettleMs = to_ms(config.workerPool.registerSettleMs);
            wc.turnsDir = "data/worker-turns";
            wc.promptsDir = "data/worker-prompts";
            wc.workers = workers;
            worker_pool = std::make_shared<persona_engine::worker_pool::WorkerPool>(wc, env::cwd());
            api::set_worker_pool(worker_pool);
            api::set_worker_pool_control(worker_pool);
            worker_persona_seeds = persona_engine::worker_pool::build_worker_persona_seeds(workers);
            // boot 自動 spawn は config.workerPool.enabled が true の時だけ (既定 false)。
            if (config.workerPool.enabled) worker_pool->start();
            // worker 未起動のペルソナは既存どおり claude -p で動作させる。
            persona_engine::ClaudeCliClientOptions fo;
            fo.defaultTimeoutMs = to_ms(config.llm.claudeCliTimeoutMs);
            fo.gitBashPath = git_bash;
            llm = std::make_shared<persona_engine::worker_pool::WorkerPoolClient>(
                worker_pool, std::make_shared<persona_engine::ClaudeCliClient>(fo));
            log::info("  persona-engine LLM: WorkerPoolClient (定義 " + std::to_string(workers.size()) + " / boot自動起動=" +
                      (config.workerPool.enabled ? "true" : "false") + ", 未起動は claude -p フォールバック)");
        } else if (config.llm.backend == "local") {
            persona_engine::LocalOpenAiClientOptions o;
            o.baseUrl = config.llm.local.baseUrl;
            o.defaultModel = config.llm.local.model;
            o.apiKey = config.llm.local.apiKey;
            o.defaultTimeoutMs = to_ms(config.llm.local.timeoutMs);
            llm = std::make_shared<persona_engine::LocalOpenAiClient>(o);
            log::info("  persona-engine LLM: LocalOpenAiClient (" + config.llm.local.baseUrl + " / model=" + config.llm.local.model + ")");
        } else if (config.llm.backend == "claude-cli") {
            persona_engine::ClaudeCliClientOptions o;
            o.defaultTimeoutMs = to_ms(config.llm.claudeCliTimeoutMs);
            o.defaultModel = config.llm.model;
            o.gitBashPath = config.llm.gitBashPath;
            llm = std::make_shared<persona_engine::ClaudeCliClient>(o);
            log::info("  persona-engine LLM: ClaudeCliClient (Lictor 経由 spawn)");
        } else if (config.llm.anthropicApiKey && !config.llm.anthropicApiKey->empty()) {
            // TS はここで AnthropicSdkClient (HTTP) を作っていた。C++ 版は API キーで直接呼ぶ経路を持たない。
            log::warn("  persona-engine LLM: backend=" + config.llm.backend +
                      " + anthropicApiKey (AnthropicSdkClient) は C++ 版では使えないため ClaudeCliClient (claude -p) を使う");
            persona_engine::ClaudeCliClientOptions o;
            o.defaultTimeoutMs = to_ms(config.llm.claudeCliTimeoutMs);
            o.defaultModel = config.llm.model;
            o.gitBashPath = config.llm.gitBashPath;
            llm = std::make_shared<persona_engine::ClaudeCliClient>(o);
            log::info("  persona-engine LLM: ClaudeCliClient (Lictor 経由 spawn)");
        } else {
            log::info("  persona-engine: skipped (set llm.backend=claude-cli or llm.anthropicApiKey)");
            return std::nullopt;
        }
        auto_discussion_llm = llm;

        // 分類器専用 LLM を persona engine とは別建てで構築する (WorkerPoolClient は personaId 前提のため)。
        classifier_llm = build_classifier_llm();

        try {
            PersonaLifecycle pl;
            // worker-pool は persona DB だけ分離し (8 固定キャスト)、workspace は実フォーラムと共有する。
            const std::string pe_db_path = is_worker_pool ? "./data/persona-engine-debate.db" : config.personaEngine.dbPath;
            const std::string workspace_id = config.workspace;
            pl.peDb = sql::Database::open(pe_db_path);
            pl.core = std::shared_ptr<core::Core>(open_active_kg(config));
            discord_hook::ensure_reaction_tables(pl.core->raw());
            auto relay_personas = std::make_shared<persona_engine::db::PersonasRepo>(pl.peDb);
            // persona の人間名を解決する。全 persona (進行役 facilitator 含む) を webhook で人間名として投稿する。
            auto resolve_speaker = [relay_personas](const std::string& by_persona_id) {
                std::string name = by_persona_id;
                try {
                    if (auto p = relay_personas->get(by_persona_id)) name = p->display_name;
                } catch (const std::exception&) {
                }
                return name;
            };
            std::weak_ptr<core::Core> weak_core = pl.core;
            discatier_engine_adapter::DiscatierAdapterOptions ao;
            // PR-I: AI 発話 / hypothesis を Discord channel に post (投稿は背景で。議論のスレッドを塞がない)。
            ao.onPostedUtterance = [resolve_speaker, weak_core](const discatier_engine_adapter::PostedUtterance& input) {
                const std::string name = resolve_speaker(input.byPersonaId);
                // ログにも人間名を反映 (誰が何を言ったか追える)。
                log::info("  [議論] " + name + ": " + str::js_slice(str::collapse_whitespace(input.text), 0, 80));
                bool queued = background_executor().submit([name, input, weak_core] {
                    auto core = weak_core.lock();
                    if (!core) return;
                    discord_hook::DiscussionPostArgs a;
                    a.core = core.get();
                    a.workspaceId = input.workspaceId;
                    a.sessionId = input.sessionId;
                    a.kind = "utterance";
                    a.speakerLabel = name;
                    a.viaWebhook = true;
                    a.text = input.text;
                    auto r = discord_hook::post_discussion_to_discord(a);
                    if (!r.ok && !r.skipped) {
                        log::warn("  persona-engine: discord utterance post skipped (" + name + "): " + r.reason.value_or(""));
                    } else if (r.messageId) {
                        discord_hook::record_posted_message(core->raw(), {*r.messageId, input.utteranceId, "utterance", r.channelId, std::nullopt});
                    }
                });
                if (!queued) log::warn("  persona-engine: discord utterance post skipped (" + name + "): queue full");
            };
            ao.onPostedHypothesis = [resolve_speaker, weak_core, workspace_id](const discatier_engine_adapter::PostedHypothesis& input) {
                bool queued = background_executor().submit([resolve_speaker, weak_core, workspace_id, input] {
                    auto core = weak_core.lock();
                    if (!core) return;
                    // hypothesis は対応する gap session の scene が discord 由来でない場合スキップ。
                    auto row = core->raw().get(
                        "SELECT id, scene FROM sessions WHERE workspace_id = ? AND title = ? ORDER BY started_at DESC LIMIT 1",
                        {workspace_id, "discussion-of-gap:" + input.designGapId.value_or("_none")});
                    if (!row) return;
                    const std::string session_id = row->str("id");
                    const auto scene = row->opt_str("scene");
                    if (!scene || !str::starts_with(*scene, "discord:")) return;
                    const std::string name = resolve_speaker(input.byPersonaId);
                    discord_hook::DiscussionPostArgs a;
                    a.core = core.get();
                    a.workspaceId = input.workspaceId;
                    a.sessionId = session_id;
                    a.kind = "hypothesis";
                    a.speakerLabel = name;
                    a.viaWebhook = true;
                    a.text = input.statement;
                    auto r = discord_hook::post_discussion_to_discord(a);
                    if (!r.ok) {
                        log::warn("  persona-engine: discord hypothesis post skipped: " + r.reason.value_or(""));
                    } else if (r.messageId) {
                        discord_hook::record_posted_message(core->raw(), {*r.messageId, input.hypothesisId, "hypothesis", r.channelId, std::nullopt});
                    }
                });
                if (!queued) log::warn("  persona-engine: discord hypothesis post skipped: queue full");
            };
            pl.adapter = discatier_engine_adapter::create_discatier_context_provider(*pl.core, std::move(ao));
            // 参加 persona の id 集合 (excludeProviders 適用後)。除外された persona を target にする rule は seed しない。
            std::set<std::string> active_worker_ids;
            if (worker_persona_seeds)
                for (const auto& p : *worker_persona_seeds) active_worker_ids.insert(p.id);
            persona_engine::PersonaEngineOptions eo;
            eo.db = pl.peDb;
            eo.llm = llm;
            eo.contextProvider = pl.adapter;
            eo.workspaceId = workspace_id;
            eo.personaSeeds = worker_persona_seeds;
            if (is_worker_pool) {
                std::vector<persona_engine::RuleSeed> seeds;
                for (const auto& r : persona_engine::worker_pool::DEBATE_RULE_SEEDS())
                    if (!r.target || active_worker_ids.count(*r.target)) seeds.push_back(r);
                auto rs = runtime_settings;
                eo.ruleSeeds = persona_engine::worker_pool::apply_rule_instruction_overrides(
                    seeds, [rs](const std::string& id) -> std::optional<std::string> { return rs->get_rule_instruction(id); });
            }
            eo.maxFiresPerSession = static_cast<std::int64_t>(config.personaEngine.maxFiresPerSession);
            eo.maxFiresPerRulePerSession = static_cast<std::int64_t>(config.personaEngine.maxFiresPerRule);
            eo.tickMs = to_ms(config.personaEngine.tickMs);
            pl.engine = persona_engine::create_persona_engine(eo);
            discatier_engine_adapter::EventBridgeOptions bo;
            bo.workspaceId = workspace_id;
            bo.pollMs = to_ms(config.personaEngine.bridgePollMs);
            pl.bridge = discatier_engine_adapter::create_event_bridge(*pl.core, discatier_engine_adapter::bridge_engine_of(pl.engine), bo);
            // excludeProviders で外した persona の rule が既存 DB に残っていれば disable する。
            if (is_worker_pool && !config.workerPool.excludeProviders.empty()) {
                int disabled_count = 0;
                persona_engine::db::RuleListFilter f;
                f.enabled = true;
                for (const auto& r : pl.engine->rules().list(f)) {
                    if (r.target && !active_worker_ids.count(*r.target)) {
                        pl.engine->rules().set_enabled(r.id, false);
                        ++disabled_count;
                    }
                }
                if (disabled_count > 0) log::info("  worker-pool: 除外 provider の rule " + std::to_string(disabled_count) + " 件を無効化");
            }
            api::set_persona_engine(pl.engine);
            // 旧 persona-engine は既定で起動しない (議論は新フロー src/flow に集約済み)。
            if (config.personaEngine.enabled) {
                pl.engine->start();
                pl.bridge->start();
                log::info("  persona-engine: attached (workspace=" + workspace_id + ", db=" + pe_db_path + ")");
            } else {
                log::info("  persona-engine: disabled (新フロー集約のため起動しない / DISCUTERE_PERSONA_ENGINE_ENABLED=1 で有効化)");
            }

            // ファシリテーター: worker-pool 時は personaId を持たない単発生成なので直 claude -p を使う。
            LLMClientPtr facilitator_llm = llm;
            if (is_worker_pool) {
                persona_engine::ClaudeCliClientOptions o;
                o.defaultTimeoutMs = to_ms(config.llm.claudeCliTimeoutMs);
                o.gitBashPath = git_bash;
                o.defaultModel = non_empty(config.llm.model);
                facilitator_llm = std::make_shared<persona_engine::ClaudeCliClient>(o);
            }
            if (config.personaEngine.enabled && config.facilitator.enabled && facilitator_llm) {
                persona_engine::facilitator::FacilitatorDeps fd;
                fd.core = pl.core;
                fd.llm = facilitator_llm;
                fd.contextProvider = pl.adapter;
                fd.personas = std::make_shared<persona_engine::db::PersonasRepo>(pl.peDb);
                fd.workspaceId = workspace_id;
                fd.logger = std::make_shared<PrefixLogger>("facilitator");
                fd.options.tickMs = config.facilitator.tickMs;
                fd.options.idleGapMs = config.facilitator.idleGapMs;
                fd.options.maxPersonas = config.facilitator.maxPersonas;
                fd.options.aufhebungTarget = config.facilitator.aufhebungTarget;
                fd.options.model = config.llm.model;
                fd.options.tierModels = persona_engine::TierModels{config.llm.tiers.cheap, config.llm.tiers.strong};
                // 収束トリガーは tick ごとに runtime-settings から読む (チューニング UI の変更を再起動なしで反映)。
                auto rs = runtime_settings;
                fd.tuning = [rs] { return rs->get_facilitator_tuning(); };
                // 収束したらフォーラムポストを締める (gateway 起動後に結線される)。
                fd.onConverged = [gateway_slot](const persona_engine::facilitator::FacilitatorConvergedEvent& e) {
                    std::shared_ptr<discord_hook::DiscordGateway> gw;
                    {
                        std::lock_guard lock(gateway_slot->mu);
                        if (!gateway_slot->forum_finalizer) return;
                        gw = gateway_slot->gateway;
                    }
                    if (!gw) return;
                    auto scene = e.scene;
                    auto summary = e.summary;
                    auto title = e.title;
                    bool queued = background_executor().submit([gw, scene, summary, title] {
                        try {
                            auto r = gw->finalize_forum_post(scene, summary, title);
                            if (r.closed) log::info("  discord-forum: post closed (converged)");
                        } catch (const std::exception& ex) {
                            log::warn("  discord-forum: finalize error: " + what_of(ex));
                        }
                    });
                    if (!queued) log::warn("  discord-forum: finalize error: queue full");
                };
                pl.facilitator = persona_engine::facilitator::create_facilitator(std::move(fd));
                pl.facilitator->start();
                log::info("  facilitator: started (tick=" + js_number_to_string(config.facilitator.tickMs) + "ms, idleGap=" +
                          js_number_to_string(config.facilitator.idleGapMs) + "ms, maxPersonas=" +
                          js_number_to_string(config.facilitator.maxPersonas) + ", workerPool=" + (is_worker_pool ? "true" : "false") + ")");
                api::set_facilitator(pl.facilitator);
            }

            // FEATURE ③+④: 合意スコアラー (react は gateway 起動後に結線される)。
            if (config.personaEngine.enabled && config.facilitator.enabled && facilitator_llm) {
                persona_engine::scoring::ConsensusScorerDeps cd;
                cd.core = pl.core;
                cd.llm = facilitator_llm;
                cd.intervalMs = config.facilitator.tickMs;
                cd.model = is_worker_pool ? non_empty(config.llm.model) : config.llm.model;
                cd.react = [gateway_slot](const persona_engine::scoring::ConsensusReactInput& i) {
                    if (auto gw = gateway_slot->get()) gw->react_to_message(i.channelId, i.messageId, i.emoji);
                };
                cd.logger = std::make_shared<PrefixLogger>("consensus");
                pl.consensusScorer = persona_engine::scoring::create_consensus_scorer(std::move(cd));
                pl.consensusScorer->start();
                log::info("  consensus-scorer: started (interval=" + js_number_to_string(config.facilitator.tickMs) + "ms)");
            }

            // 自動シード議論: facilitator が動いている時だけ (worker-pool 時は起動しない)。
            if (config.autoSeed.enabled && pl.facilitator && !is_worker_pool) {
                discussion_seed::AutoSeedSchedulerDeps sd;
                sd.core = pl.core;
                sd.personas = std::make_shared<persona_engine::db::PersonasRepo>(pl.peDb);
                sd.workspaceId = workspace_id;
                sd.config.intervalMs = config.autoSeed.intervalMs;
                sd.config.maxConcurrent = config.autoSeed.maxConcurrent;
                sd.config.sources = config.autoSeed.sources;
                sd.logger = std::make_shared<PrefixLogger>("auto-seed");
                pl.autoSeed = discussion_seed::create_auto_seed_scheduler(std::move(sd));
                pl.autoSeed->start();
                log::info("  auto-seed: started (interval=" + js_number_to_string(config.autoSeed.intervalMs) + "ms, maxConcurrent=" +
                          js_number_to_string(config.autoSeed.maxConcurrent) + ", sources=" + str::join(config.autoSeed.sources, "/") + ")");
            }
            return pl;
        } catch (const std::exception& e) {
            log::warn("  persona-engine: startup failed: " + what_of(e));
            return std::nullopt;
        }
    }();

    // ─── ゲーム経済グラフ (GET /ludus/economy-graph/:slug, POST /api/ludus/analyze-economy) ───
    // C++ 版は API キーを持たず claude -p (LLMClient) で分析する (w3-K)。
    ludus::AnalyzeEconomyDeps economy_deps;
    {
        persona_engine::ClaudeCliClientOptions o;
        o.defaultTimeoutMs = to_ms(config.llm.claudeCliTimeoutMs);
        o.gitBashPath = git_bash;
        economy_deps.llm = std::make_shared<persona_engine::ClaudeCliClient>(o);
        api::EconomyGraphRoutesDeps ed;
        ed.workspaceId = config.workspace;
        ed.analyze = economy_deps;
        app->mount("/", api::create_economy_graph_routes(std::move(ed)));
        log::info("  economy-graph: /ludus/economy-graph/:slug — ゲームメカニクス経済グラフ");
    }

    // 分類器 starter (gateway と web-chat で共有。classifier LLM は起動後に変わらない)。
    auto make_starter = [&classifier_llm]() {
        discord_hook::DiscordAutoDiscussionStarterOptions so;
        LLMClientPtr c = classifier_llm;
        so.getLlm = [c] { return c; };
        return discord_hook::create_discord_auto_discussion_starter(std::move(so));
    };

    // ─── 軽量 Web チャット UI (/chat) — Discord 非依存の議論経路 ───
    {
        std::shared_ptr<persona_engine::db::PersonasRepo> web_personas;
        if (persona_lifecycle) web_personas = std::make_shared<persona_engine::db::PersonasRepo>(persona_lifecycle->peDb);
        api::WebChatDeps wd;
        wd.workspaceId = config.workspace;
        wd.classifyInboundMessage = make_starter();
        wd.resolveSpeakerName = [web_personas](const std::string& persona_id) {
            if (web_personas) {
                try {
                    if (auto p = web_personas->get(persona_id)) return p->display_name;
                } catch (const std::exception&) {
                }
            }
            return persona_id;
        };
        api::set_web_chat_deps(std::move(wd));
        app->mount("/", api::web_chat_routes());
        log::info("  web-chat: /chat (loopback) — scene=web:<room> で議論可能");
    }

    // ─── 議論フローの LLM 解決 (worker-pool 踏襲 + 無ワーカー時 claude -p フォールバック) ───
    LLMClientPtr flow_claude_llm;
    if (auto_discussion_llm && config.llm.backend == "worker-pool") {
        persona_engine::ClaudeCliClientOptions o;
        o.defaultTimeoutMs = to_ms(config.llm.claudeCliTimeoutMs);
        o.defaultModel = non_empty(config.llm.model);
        o.gitBashPath = git_bash;
        // 鎖: worker-pool (personaId ルート) → claude -p。TS の SDK (OAuth/cache) 段は C++ 版では持たない。
        flow_claude_llm = std::make_shared<flow::FallbackLlm>(auto_discussion_llm, std::make_shared<persona_engine::ClaudeCliClient>(o));
    } else {
        flow_claude_llm = auto_discussion_llm ? auto_discussion_llm : classifier_llm;
    }
    // model spec が `gpt-*` のペルソナは Codex CLI (`codex exec`) へ、それ以外は Claude チェーンへ振り分ける。
    LLMClientPtr flow_engine_llm;
    if (flow_claude_llm) {
        persona_engine::CodexCliClientOptions co;
        co.cwd = fs::join(env::cwd(), "worker-home");
        flow_engine_llm = std::make_shared<flow::ModelRouterLlm>(flow_claude_llm, std::make_shared<persona_engine::CodexCliClient>(co));
    }
    auto open_core = [&config]() { return open_active_kg(config); };
    const std::function<std::optional<std::string>()> youtube_key = [] { return secrets::get_youtube_api_key(); };

    // ─── 議論フロー WebUI (/flow) — LLM backend がある時のみ有効 ───
    if (flow_engine_llm) {
        flow::web::FlowWebDeps fw;
        fw.workspaceId = config.workspace;
        fw.llm = flow_engine_llm;
        fw.openCore = open_core;
        // 外部の声 RAG + 情報ゲートの密度評価に active KG の実材料を流す。
        fw.listExternalVoices = flow::make_list_external_voices(open_core, config.workspace);
        fw.sentimentClients = sentiment_clients_of(flow_engine_llm);
        fw.getYoutubeApiKey = youtube_key;
        flow::web::set_flow_web_deps(std::move(fw));
        app->mount("/", flow::web::flow_routes());
        app->mount("/", flow::web::spec_extract_routes());
        log::info("  flow-ui: /flow (loopback) — 議論/改善/学習/壁打ち を起動");
    }

    const double port = config.server.port;
    // This endpoint returns proposals only; Actio owns transport and human task approval.
    const std::string external_secret = config.externalDiscussion.sharedSecret;
    std::string external_model;
    if (!config.flow.roster.discussants.empty() && !config.flow.roster.discussants[0].empty()) external_model = config.flow.roster.discussants[0];
    else external_model = config.llm.model.value_or("");
    if (!external_secret.empty() && external_model.empty()) {
        log::error("External discussion requires a configured model");
        vestigium->shutdown();
        return 1;
    }
    {
        std::shared_ptr<external_discussion::ExternalDiscussionService> service;
        if (!external_secret.empty() && !external_model.empty() && flow_engine_llm) {
            service = std::make_shared<external_discussion::ExternalDiscussionService>(
                std::make_shared<external_discussion::ParticipationStore>(flow::db::get_flow_db()), flow_engine_llm, external_model);
        }
        app->mount("/internal/external-discussion", api::external_discussion_routes(service, external_secret));
    }

    // ─── S3 バックアップ: 月次自動スケジューラ起動 (enabled かつ bucket 設定時のみ) ──
    std::shared_ptr<backup::BackupSchedulerHandle> backup_scheduler = backup::start_backup_scheduler(config);

    // ─── LLM コストの定期 relay ── 有効化/送信先/service は runtime-settings から毎 tick 読む。
    std::function<void()> stop_cost_relay;
    {
        flow::StartCostRelayOptions co;
        co.intervalMs = to_ms(config.cost.relay.intervalMs);
        auto rs = runtime_settings;
        co.resolve = [rs, &config]() {
            runtime_settings::CostRelaySettings s;
            if (rs) {
                s = rs->get_cost_relay();
            } else {
                s.enabled = config.cost.relay.enabled;
                s.anatomiaUrl = config.cost.relay.anatomiaUrl.value_or("");
                s.concordiaUrl = config.cost.relay.concordiaUrl.value_or("");
                s.service = config.cost.relay.service;
            }
            flow::CostRelayLive live;
            live.enabled = s.enabled;
            live.service = s.service;
            live.targets = flow::build_cost_feed_targets({s.anatomiaUrl, s.concordiaUrl});
            return live;
        };
        stop_cost_relay = flow::start_cost_relay(flow::db::get_flow_db(), std::move(co));
        std::string where = "(none)";
        bool enabled = config.cost.relay.enabled;
        if (runtime_settings) {
            auto s = runtime_settings->get_cost_relay();
            enabled = s.enabled;
            std::vector<std::string> labels;
            for (const auto& t : flow::build_cost_feed_targets({s.anatomiaUrl, s.concordiaUrl})) labels.push_back(t.label.value_or(t.baseUrl));
            where = labels.empty() ? "(none)" : str::join(labels, ", ");
        }
        log::info("  cost-relay: timer started (interval=" + js_number_to_string(config.cost.relay.intervalMs) + "ms, enabled=" +
                  (enabled ? "true" : "false") + ", -> " + where + "); 設定は /api/admin/tuning でライブ編集可");
    }

    // 議論キューのサマリ生成 (slash /discutere-queue 用)。core は都度 open/close。
    sql::DatabasePtr pe_db_for_queue = persona_lifecycle ? persona_lifecycle->peDb : nullptr;
    auto build_queue_text = [&config, pe_db_for_queue]() {
        auto core = open_active_kg(config);
        std::string text;
        try {
            text = queue::format_queue_text(queue::build_queue_snapshot(core->raw(), pe_db_for_queue.get(), config.workspace));
        } catch (...) {
            core->close();
            throw;
        }
        core->close();
        return text;
    };

    // ゲーム感想の匿名収集ストア (専用 sqlite)。
    auto game_feedback_store = std::make_shared<feedback::GameFeedbackStore>(sql::Database::open("./data/game-feedback.db"));

    // WS: Discord Gateway 常時接続 (公開 URL + 署名検証エンドポイント不要)。botToken 未設定なら skip。
    discord_hook::DiscordGatewayDeps gd;
    gd.botToken = config.discord.botToken.value_or("");
    gd.applicationId = config.discord.applicationId;
    gd.guildIds = config.discord.guildIds;
    gd.router.workspaceId = config.workspace;
    gd.router.adminIds = config.discord.adminIds;
    gd.router.discussionChannelIds = config.discord.discussionChannelIds;
    // データクロール用チャンネル: 貼られた URL から外部議論データを取り込む。
    gd.crawlChannelIds = config.discord.crawlChannelIds;
    // フォーラム集約: guild ごとに解決した Di 管理 forum ID だけを監視し、運用出力チャンネルを自動作成。
    gd.forum = config.discord.forum;
    if (flow_engine_llm) {
        auto deps = std::make_shared<design_diagnostic::DiagnosticDependencies>();
        deps->llm = flow_engine_llm;
        gd.mechanicsCheck = deps;
        gd.levelCheck = deps;
        // 新フロー (議論/改善/学習/壁打ち) の Discord live 実行依存。
        flow::FlowDiscordDeps fl;
        fl.llm = flow_engine_llm;
        fl.openCore = open_core;
        fl.listExternalVoices = flow::make_list_external_voices(open_core, config.workspace);
        fl.sentimentClients = sentiment_clients_of(flow_engine_llm);
        fl.workspaceId = config.workspace;
        fl.getYoutubeApiKey = youtube_key;
        gd.flowLive = std::move(fl);
    }
    // フォーラム starter の解析 (flow/entry-discord)。
    gd.entryParsers.parseForumEntry = [](const std::vector<std::string>& names) {
        auto p = flow::parse_forum_entry(names);
        discord_hook::ParsedForumEntry out;
        out.flow = p.flow;
        out.tags = p.tags;
        return out;
    };
    gd.entryParsers.parseRoundsTurns = [](const std::string& text) {
        auto r = flow::parse_rounds_turns(text);
        return discord_hook::ParsedRoundsTurns{r.rounds, r.turnsPerRound};
    };
    gd.entryParsers.parseOpponents = [](const std::string& text) { return flow::parse_opponents(text); };
    // /debate のパーティ議論 (司会+キーマン+意見、想定発話数で続行/停止)。
    gd.debate = config.discussion;
    gd.gitBashPath = git_bash;
    // ゲーム感想チャンネル: カテゴリ「ゲーム感想」配下の投稿を匿名で意見データ収集。
    gd.gameFeedback = config.discord.gameFeedback;
    gd.onGameFeedback = [game_feedback_store](const discord_hook::GameFeedbackInput& fb) {
        auto r = game_feedback_store->add(fb.gameTitle, fb.content, static_cast<std::int64_t>(fb.createdAt));
        if (r) log::info("  game-feedback: 収集 [" + fb.gameTitle + "] " + str::js_slice(str::collapse_whitespace(fb.content), 0, 60));
    };
    {
        discord_hook::CrawlDeps cd;
        cd.createCore = open_core;
        cd.workspaceId = config.workspace;
        cd.getYoutubeApiKey = youtube_key;
        cd.reddit = flow::reddit_credentials_from_env();
        // website 長文の要約器 (id=67)。LLM があれば要約/raw 2 層で取り込む。
        if (auto_discussion_llm) {
            crawler::sources::LlmSummarizerOptions so;
            so.model = config.llm.model;
            cd.summarizer = crawler::sources::create_llm_summarizer(auto_discussion_llm, so);
        }
        gd.crawlDeps = std::move(cd);
    }
    gd.router.getEngine = [] { return api::get_persona_engine(); };
    gd.router.buildQueueText = build_queue_text;
    gd.router.triggerBackup = [backup_scheduler] { return backup_scheduler->trigger(); };
    const int port_int = static_cast<int>(port);
    gd.router.triggerEconomyAnalysis = [&config, economy_deps, port_int](const std::string& game_title) {
        if (!ludus::analyze_economy_in_background(config.workspace, game_title, economy_deps))
            log::error("  economy-graph: analysis failed: queue full or already running");
        return discord_hook::EconomyAnalysisStarted{ludus::to_slug(game_title), port_int};
    };
    // 憑依対象ペルソナの生成 (item7): クロール済み外部発話から C1 採用して flow_persona へ upsert。
    gd.router.triggerPersonaGenerate = [](const std::optional<std::string>& source) {
        flow::AdoptRunOptions o;
        o.sourceFilter = source;
        o.dry = false;
        auto summary = flow::run_adopt_from_kg(o);
        return discord_hook::PersonaGenerateResult{summary.adopted, summary.speakers};
    };
    // 分類器は専用 LLM (Haiku / Lictor)。persona engine の summarizer とは別系統。
    gd.router.classifyInboundMessage = make_starter();
    // 議論意見へのリアクション → 内部スコア加算 (絵文字ごとの重み)。
    std::shared_ptr<core::Core> pe_core = persona_lifecycle ? persona_lifecycle->core : nullptr;
    gd.onReaction = [pe_core](const discord_hook::ReactionInfo& info) {
        if (!pe_core) return;
        try {
            discord_hook::ReactionInput in;
            in.messageId = info.messageId;
            in.emoji = info.emoji;
            discord_hook::apply_reaction(pe_core->raw(), in);
        } catch (const std::exception& e) {
            log::warn("  discord-gateway: reaction scoring failed: " + what_of(e));
        }
    };

    // Gateway の起動は専用スレッドで (TS の Promise。HTTP の listen を待たせない)。
    // 起動後に合意スコアラーの 👍 / 未検知投稿の再スイープ / フォーラム収束 finalizer を結線する。
    const bool forum_enabled = config.discord.forum.enabled;
    std::thread gateway_thread([gd = std::move(gd), gateway_slot, forum_enabled]() mutable {
        std::shared_ptr<discord_hook::DiscordGateway> handle;
        try {
            handle = discord_hook::start_discord_gateway(std::move(gd));
        } catch (const std::exception& e) {
            log::warn("  discord-gateway: startup failed: " + what_of(e));
            return;
        }
        if (!handle) return;
        {
            std::lock_guard lock(gateway_slot->mu);
            gateway_slot->gateway = handle;
        }
        // FEATURE ⑤b: 未検知投稿の再スイープを admin API (/api/admin/seed-sweep) に結線。
        std::weak_ptr<discord_hook::DiscordGateway> weak = handle;
        api::set_gateway_sweeper([weak](std::optional<double> limit) {
            auto gw = weak.lock();
            if (!gw) throw std::runtime_error("discord gateway stopped");
            std::optional<int> l;
            if (limit) l = static_cast<int>(*limit);
            auto r = gw->sweep_unseeded(l);
            return api::SweepResult{static_cast<double>(r.scanned), static_cast<double>(r.seeded)};
        });
        api::set_gateway_thread_starter([weak](const std::string& thread_id) {
            auto gw = weak.lock();
            if (!gw) throw std::runtime_error("discord gateway stopped");
            const auto r = gw->start_existing_thread(thread_id);
            return api::StartThreadResult{r.queued, r.reason};
        });
        log::info("  discord-seed-sweep / consensus-react: wired to gateway");
        if (!forum_enabled) return;
        {
            std::lock_guard lock(gateway_slot->mu);
            gateway_slot->forum_finalizer = true;
        }
        log::info("  discord-forum: convergence finalizer wired");
    });

    // ─── Slack Socket Mode 起動 (Discord と並ぶ第2トランスポート) ───
    //   appToken/botToken/channelIds が揃い enabled かつ LLM backend がある時のみ起動。
    std::shared_ptr<slack::SocketModeHandle> slack_socket;
    if (config.slack.enabled && !config.slack.appToken.empty() && !config.slack.botToken.empty() && !config.slack.channelIds.empty() &&
        flow_engine_llm) {
        slack::SlackRouterConfig sc;
        sc.channelIds = config.slack.channelIds;
        sc.deps.botToken = config.slack.botToken;
        sc.deps.llm = flow_engine_llm;
        sc.deps.openCore = open_core;
        sc.deps.sentimentClients = sentiment_clients_of(flow_engine_llm);
        sc.deps.workspaceId = config.workspace;
        auto router = slack::create_slack_router(std::move(sc));
        slack::SocketModeDeps sd;
        sd.appToken = config.slack.appToken;
        sd.onEvent = router.onEvent;
        sd.onInteractive = router.onInteractive;
        slack_socket = slack::start_socket_mode(std::move(sd));
        log::info("  slack: Socket Mode 起動 (議論/改善/学習/壁打ち)");
    }

    const std::string port_text = js_number_to_string(port);
    log::info("Discutere listening on http://localhost:" + port_text);
    log::info("  Auth:     Discord Gateway (bot token + admin-id allowlist) / HTTP は X-User-Id・X-User-Role ヘッダー");
    log::info("  Tasks:    /api/groups/:id/tasks");
    log::info("  Monitors: /api/groups/:id/monitors");
    log::info("  Webhooks: /api/webhook/slack, /api/webhook/discord");
    // TS の文字列のまま (元のソースの文字化け "\u2001E" も同じバイト列で出す)。
    log::info("  Discord:  Gateway (WS) \xE2\x80\x81" "Eslash + message via bot token");
    log::info("  Admin:    /api/admin/{rules/enabled,session/reset,status}");
    log::info("  Dashboard: /api/admin/dashboard (HTML, admin role)");
    log::info("  Learning:  /learning (local read-only view)");
    log::info("  Analyze:  /api/analyze");
    if (is_worker_pool) log::info("  WorkerPool: /internal/worker/{register,utterance} (常駐ワーカー callback)");

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    // 既定は loopback (127.0.0.1) bind。LAN bind が要る場合のみ BACKEND_HOST=0.0.0.0 等で上書き (config.server.host)。
    http::ServerOptions options;
    options.host = config.server.host;
    options.port = static_cast<std::uint16_t>(port_int);
    auto server = std::make_unique<http::Server>(options, app);
    try {
        server->start();
    } catch (const std::exception& e) {
        log::error("[discutere] HTTP listen failed on " + options.host + ":" + port_text + ": " + what_of(e));
        std::fflush(nullptr);
        std::_Exit(1);
    }
    log::info("[discutere] HTTP listening on " + options.host + ":" + std::to_string(server->bound_port()));

    while (g_signal == 0) std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // 終了: HTTP を止めてから常駐ワーカー・タイマー・接続を止める (TS の gracefulShutdown + C++ で増えた後始末)。
    log::info("\n" + signal_name(static_cast<int>(g_signal)) + " received — shutting down (worker pool stop)...");
    // 後始末が固まっても終われるよう、全体に期限を付ける (TS は process.exit で即終了していた)。
    std::thread watchdog([] {
        std::this_thread::sleep_for(std::chrono::seconds(20));
        std::fflush(nullptr);
        std::_Exit(0);
    });
    watchdog.detach();
    try {
        server->stop();
    } catch (...) {
    }
    try {
        if (worker_pool) worker_pool->stop();
    } catch (const std::exception& e) {
        log::warn("  worker-pool stop failed: " + what_of(e));
    }
    try {
        stop_kg_sync();
    } catch (...) {
    }
    try {
        if (slack_socket) slack_socket->stop();
    } catch (...) {
    }
    if (gateway_thread.joinable()) gateway_thread.join();
    try {
        if (auto gw = gateway_slot->get()) gw->stop();
    } catch (...) {
    }
    try {
        if (stop_cost_relay) stop_cost_relay();
        backup_scheduler->stop();
    } catch (...) {
    }
    if (persona_lifecycle) {
        try {
            if (persona_lifecycle->autoSeed) persona_lifecycle->autoSeed->stop();
            if (persona_lifecycle->consensusScorer) persona_lifecycle->consensusScorer->stop();
            if (persona_lifecycle->facilitator) persona_lifecycle->facilitator->stop();
            persona_lifecycle->bridge->stop();
            persona_lifecycle->engine->stop();
        } catch (...) {
        }
    }
    global_scheduler().stop();
    // 走っている仕事 (LLM 待ち等) の終わりは長く待たない: 5 秒で打ち切って終了へ進む。
    {
        auto done = std::make_shared<std::atomic<bool>>(false);
        std::thread stopper([done] {
            background_executor().stop();
            done->store(true);
        });
        for (int i = 0; i < 50 && !done->load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (done->load()) stopper.join();
        else stopper.detach();
    }
    vestigium->shutdown();
    std::fflush(nullptr);
    // 静的オブジェクトの破棄順 (背景スレッドが残っている場合) に依存しないよう、ここで即終了する (TS の process.exit(0))。
    std::_Exit(0);
}

} // namespace

int main() {
    try {
        return run_server();
    } catch (const std::exception& e) {
        di::log::error(std::string("[discutere] fatal: ") + e.what());
        std::fflush(nullptr);
        return 1;
    }
}
