#pragma once
// 外部の声の埋め込みインデックス書き込み (src/core/vectors/voice-index.ts, spec/feature/voice-rag-hybrid.md)。
//
// offline バッチ (`scripts/build-voice-embeddings.ts`) と、議論準備中の収集直後の
// 部分ベクトル化 (`flow/user-voices/collect`) の両方がこの関数で書き込む。
// 1 バッチ 1 トランザクションで upsert するので、途中で止まっても再実行で続きから埋まる。
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "core/index.hpp"
#include "core/vectors/embedder.hpp"

namespace di::core::vectors {

/** 埋め込み対象テキストの上限 (長文はモデルの実効長に合わせて先頭を使う。UTF-16 単位)。 */
inline constexpr std::size_t EMBED_TEXT_CAP = 2000;
/** embed_voices_by_ids が 1 回に受ける id 数の上限 (移植で追加。超えた分は埋めない)。 */
inline constexpr std::size_t kEmbedVoiceIdsMax = 20'000;

struct VoiceRow {
    std::string id;
    std::optional<std::string> raw_content;
};

struct EmbedVoiceOptions {
    std::string workspaceId;
    int batchSize = 1;
    std::function<void(std::size_t done, std::size_t total)> onProgress;
};

/**
 * 行を埋め込んで embeddings (nodeType='utterance') に upsert する。@returns 書き込んだ件数。
 * embedder が返したベクトルの数が足りなければ std::runtime_error (TS は undefined で登録に失敗する)。
 */
std::size_t embed_voice_rows(Core& core, EmbeddingClient& embedder, const std::vector<VoiceRow>& rows, const EmbedVoiceOptions& opts);

/**
 * 指定 id の外部の声のうち、まだベクトルが無いものだけを埋め込む (収集直後の部分ベクトル化)。
 * @returns 書き込んだ件数。
 */
std::size_t embed_voices_by_ids(Core& core, EmbeddingClient& embedder, const std::vector<std::string>& ids, const EmbedVoiceOptions& opts);

} // namespace di::core::vectors
