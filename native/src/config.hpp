#pragma once
// Discutere サービス固有 config (config ファイル化)。移植元 src/config.ts。
//
// env 散在を単一 typed config に集約する。読み込み優先順は:
//   default < config ファイル (JSON) < env
// (= 平常運用は JSON、秘密情報/CI は env で上書き)。
//
// config ファイルパスは env `DISCUTERE_CONFIG` (既定 `./discutere.config.json`)。
// 無ければ env / default のみで動く (= 後方互換、従来の .env 運用も生きる)。
//
// フィールド名は TS の DiscutereConfig のまま (camelCase)。TS の number は double で持つ
// (config ファイルの値をそのまま写すため。整数が要る呼び出し側で丸める)。
// TS の `?:` (未定義なら JSON に出ない) は std::optional。
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "base/json.hpp"
#include "flow/vote_lenses.hpp"

namespace di::config {

/** "claude-cli" | "anthropic" | "mock" | "worker-pool" | "local" */
using LlmBackend = std::string;

/** 常駐ワーカー 1 体 (= 1 ペルソナ) の定義。 */
struct WorkerPoolWorker {
    std::string id;
    std::string role;
    std::string provider;  // "claude" | "codex"
    std::string model;
};

struct OpinionModel {
    std::string model;
    double weight = 0;
};

struct KnowledgeGraphEntry {
    std::string id;
    std::optional<std::string> label;
    std::string kuzuPath;
};

struct DiscutereConfig {
    std::string nodeEnv;
    struct {
        std::string sharedSecret;
    } externalDiscussion;
    struct {
        std::string token;
    } glabConclusions;
    struct {
        double port = 0;
        /** HTTP サーバの bind アドレス。既定 `127.0.0.1` (loopback only)。env `BACKEND_HOST`。 */
        std::string host;
        std::string frontendUrl;
    } server;
    /** 4 フロー共通の進行量設定 */
    struct Flow {
        double rounds = 0;
        double turnsPerRound = 0;
        double personaCount = 0;
        /** モデル編成 (spec/feature/flow/model-roster.md)。 */
        struct {
            std::string facilitator;
            std::vector<std::string> discussants;
        } roster;
        double voterCount = 0;
        /** 投票者に巡回割当する評価レンズ (respec 03)。env は JSON 配列文字列。 */
        std::vector<flow::VoteLens> voteLenses;
        /** 早期収束に要求する直近ラウンドの投票集中度 (既定 0.6)。 */
        double convergeShare = 0;
        /** "dialectic" | "rounds" */
        std::string engine;
        struct {
            double rebutTurns = 0;
            double exchangeTurns = 0;
            double argumentDepth = 0;
            double staleTurns = 0;
            std::string judgeModel;
            bool ruleCheck = true;
            /** 結論候補の評価・批准専用。空なら judgeModel を使う。 */
            std::string finalJudgeModel;
        } dialectic;
        std::string personaSetupModel;
        std::vector<std::string> tags;
        double sparringMaxTurns = 0;
        double youtubeMaxComments = 0;
        bool autoAdoptOnCrawl = false;
        struct {
            bool enabled = true;
            std::string source;
            std::vector<std::string> sources;
            double minVoices = 0;
            double maxItems = 0;
        } autoCrawl;
        struct {
            bool enabled = true;
            std::string minDensity;
            double maxLearnIterations = 0;
            double maxGapsPerIteration = 0;
            std::string model;
        } informationGating;
        struct {
            bool enabled = true;
            double minArmableIssues = 0;
        } debatability;
        struct {
            bool enabled = false;
            bool webCanonical = true;
            std::string model;
            double timeoutMs = 0;
        } paperReview;
        struct {
            double voices = 0;
            double mechanicsTarget = 0;
            bool enrichMechanics = true;
            std::string enrichModel;
        } paperRichness;
        struct {
            bool enabled = true;
            double steamMaxReviews = 0;
            std::vector<std::string> steamLanguages;
            double glabMaxImpressions = 0;
            std::string voluptasBaseUrl;
        } userVoices;
        struct {
            double maxDepth = 0;
            double maxPages = 0;
        } notionLinks;
        struct {
            std::string effectModel;
        } improvement;
        struct {
            bool enabled = false;
            std::string model;
        } paperRefine;
        struct {
            bool enabled = true;
            double turns = 0;  // 1..20 の整数
            std::string model;
            std::string forecastModel;
        } foresight;
        struct {
            bool enabled = false;
            double budgetTokens = 0;     // 500..maxBudgetTokens の整数
            double maxBudgetTokens = 0;  // 500..10000 の整数
            std::string stanceModel;
            bool requireEvalPassed = true;
        } proxy;
        struct {
            bool enabled = false;
            std::string binPath;
            bool autoDraft = true;
            std::string refineModel;
            double timeoutMs = 0;
        } anatomia;
    } flow;
    /** 匿名議論 workspace (個人データ非保管) */
    std::string workspace;
    struct {
        std::optional<std::string> kuzuPath;
        std::optional<std::string> activeKg;
        /** タスク別 KG レジストリ (§12)。config ファイルからのみ宣言する。 */
        std::optional<std::vector<KnowledgeGraphEntry>> knowledgeGraphs;
    } discatier;
    struct {
        bool enabled = false;
        std::string dbPath;
        double maxFiresPerSession = 0;
        double maxFiresPerRule = 0;
        double tickMs = 0;
        double bridgePollMs = 0;
    } personaEngine;
    struct {
        bool enabled = true;
        double tickMs = 0;
        double idleGapMs = 0;
        double maxPersonas = 0;
        double aufhebungTarget = 0;
    } facilitator;
    struct {
        bool enabled = false;
        double intervalMs = 0;
        double maxConcurrent = 0;
        std::vector<std::string> sources;
    } autoSeed;
    struct {
        LlmBackend backend;
        std::optional<std::string> anthropicApiKey;
        std::optional<std::string> model;
        struct {
            std::optional<std::string> cheap;
            std::optional<std::string> strong;
        } tiers;
        double claudeCliTimeoutMs = 0;
        std::optional<std::string> gitBashPath;
        struct {
            std::string baseUrl;
            std::string model;
            std::optional<std::string> apiKey;
            double timeoutMs = 0;
        } local;
    } llm;
    struct {
        bool enabled = false;
        std::string baseUrl;
        std::string model;
        std::optional<std::string> apiKey;
        double timeoutMs = 0;  // 正の整数
        double batchSize = 0;  // 正の整数
    } embedding;
    struct {
        /** "claude-cli" | "anthropic" | "local" | "off" */
        std::string backend;
        std::string model;
        double timeoutMs = 0;
    } classifier;
    struct Discussion {
        double expectedUtterances = 0;
        double keymanCount = 0;
        std::string facilitatorModel;
        std::string keymanModel;
        std::vector<OpinionModel> opinionModels;
        double minTotal = 0;
        double turnDelayMs = 0;
        double continueTimeoutMs = 0;
        double maxRounds = 0;
    } discussion;
    struct {
        bool enabled = false;
        std::string workspace;
        std::string callbackBaseUrl;
        std::optional<std::string> gitBashPath;
        double injectDelayMs = 0;
        double turnTimeoutMs = 0;
        double registerTimeoutMs = 0;
        double registerSettleMs = 0;
        std::vector<WorkerPoolWorker> workers;
        std::vector<std::string> excludeProviders;
    } workerPool;
    struct Discord {
        std::optional<std::string> botToken;
        std::optional<std::string> applicationId;
        std::vector<std::string> guildIds;
        /** admin slash の認可 allowlist。空なら全 deny (安全 default) */
        std::vector<std::string> adminIds;
        std::vector<std::string> discussionChannelIds;
        std::vector<std::string> crawlChannelIds;
        struct Forum {
            bool enabled = true;
            std::string summaryChannelName;
            std::string dataLearningChannelName;
            std::string managedCategoryName;
            std::vector<std::string> improvementTagNames;
            std::vector<std::string> funTagNames;
            std::string discussionForumName;
            /** guild ID ごとの議論フォーラム名。載っている guild だけ discussionForumName の代わりに使う。 */
            std::map<std::string, std::string> discussionForumNameByGuild;
        } forum;
        struct GameFeedback {
            bool enabled = true;
            std::string categoryName;
        } gameFeedback;
    } discord;
    struct {
        bool enabled = false;
        std::string appToken;
        std::string botToken;
        std::vector<std::string> channelIds;
        std::vector<std::string> adminIds;
    } slack;
    struct {
        bool enabled = false;
        std::optional<std::string> bucket;
        std::string region;
        std::string prefix;
        std::string storageClass;
        std::optional<std::string> endpoint;
        std::optional<std::string> accessKeyId;
        std::optional<std::string> secretAccessKey;
        double intervalDays = 0;
        std::string stateFile;
    } backup;
    struct {
        bool enabled = true;
        std::string dataDir;
    } fundamentum;
    struct {
        std::optional<std::string> sourceUrl;
        std::optional<std::string> token;
        bool auto_ = false;  // JSON 上は "auto" (C++ の予約語を避ける)
        double intervalHours = 0;
        std::string stateFile;
        std::optional<std::string> serveToken;
    } kgSync;
    struct {
        struct {
            bool enabled = false;
            std::optional<std::string> anatomiaUrl;
            std::optional<std::string> concordiaUrl;
            double intervalMs = 0;
            std::string service;
        } relay;
    } cost;
};

/** load_config の注入点 (テスト用)。既定はプロセスの環境変数と作業ディレクトリ。 */
struct LoadOptions {
    /** 環境変数 (nullopt = プロセスの環境)。DISCUTERE_CONFIG もここから読む。 */
    std::optional<std::map<std::string, std::string>> env;
    /** 相対パスの基準 (空 = プロセスの作業ディレクトリ)。TS の path.resolve の基準。 */
    std::string cwd;
};

/** config ファイルの読み込み上限 (外部入力として課金する)。 */
inline constexpr std::size_t kMaxConfigFileBytes = 4u << 20;

/**
 * TS の loadConfig。毎回読み直す。
 * 正の整数が必須な設定 (embedding.timeoutMs / batchSize) が不正なら std::runtime_error。
 */
DiscutereConfig load_config(const LoadOptions& options = {});

/** プロセス内で 1 度だけ load してキャッシュ (起動時に確定)。TS の getConfig。 */
const DiscutereConfig& get_config();

/** テスト専用: キャッシュをクリアして次回 get_config() で再ロードさせる (TS の _resetConfig)。 */
void reset_config_for_test();

/** JSON.stringify(getConfig()) と同じ形 (undefined のキーは出さない、キー順は TS のオブジェクトリテラル順)。 */
Json to_json(const DiscutereConfig& config);

} // namespace di::config
