#pragma once
// 埋め込みクライアント — OpenAI 互換 `/embeddings` を叩く (Ollama / vLLM / LM Studio)。
// (src/core/vectors/embedder.ts)
// 外部の声 RAG の意味検索用。既定は Ollama ローカル (`http://localhost:11434/v1`) の bge-m3。
// 接続先は設定で決まるローカル/社内のサービスなので allow_private_network = true で呼ぶ。
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/vectors/embed_retry.hpp"

namespace di::core::vectors {

struct EmbeddingConfig {
    /** 意味検索 (ハイブリッド RAG) を有効にするか。 */
    bool enabled = false;
    /** OpenAI 互換ベース URL (末尾 `/v1`)。 */
    std::string baseUrl;
    /** 埋め込みモデル名 (例 `bge-m3`)。 */
    std::string model;
    /** 任意。設定時のみ Bearer 認証。 */
    std::optional<std::string> apiKey;
    /** 応答待ちタイムアウト ms。 */
    double timeoutMs = 0;
    /** 一括埋め込み時の 1 リクエスト最大テキスト数。 */
    double batchSize = 0;
};

class EmbeddingClient {
public:
    virtual ~EmbeddingClient() = default;
    /** テキスト群をベクトル化する。入力順を保つ。失敗は throw (呼び出し側で degrade を判断)。 */
    virtual std::vector<std::vector<double>> embed(const std::vector<std::string>& texts) = 0;
};

struct EmbedderHooks {
    WarnFn warn;                               // 省略時 "[embedder] ..." を log::warn
    std::optional<EmbedRetryOptions> retry;    // 省略時 DEFAULT_EMBED_RETRY
    SleepFn sleep;                             // 省略時 呼んだスレッドで待つ (テスト用に差し替え可)
};

/** 応答の上限 (外部入力)。 */
struct EmbedderLimits {
    std::size_t max_response_bytes = 64u << 20;
    std::size_t max_json_elements = 20'000'000;
};

/**
 * OpenAI 互換 `/embeddings` クライアントを作る。設定が不正なら std::invalid_argument (TS と同じ文言)。
 * 一過性の失敗 (タイムアウト / ネットワーク断 / 5xx / 429) はバッチ単位で指数バックオフ再試行する。
 */
std::shared_ptr<EmbeddingClient> create_open_ai_compat_embedder(const EmbeddingConfig& config,
                                                                 EmbedderHooks hooks = {},
                                                                 EmbedderLimits limits = {});

/** 設定の endpoint URL (`<baseUrl 末尾の / を除く>/embeddings`)。不正なら std::invalid_argument。 */
std::string embedding_endpoint(const EmbeddingConfig& config);

} // namespace di::core::vectors
