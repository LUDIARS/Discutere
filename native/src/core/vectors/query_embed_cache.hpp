#pragma once
// クエリ埋め込みキャッシュ (src/core/vectors/query-embed-cache.ts) — 同期検索経路にベクトル検索を持ち込むための橋。
//
// 「クエリ (議題語) のベクトルだけ」を sidecar テーブル embedding_query_cache + メモリにキャッシュし、
//   - 同期読み (get_cached_query_vector): キャッシュ命中ならベクトルを返す
//   - 温め (warm_query_embedding): 未命中クエリを裏 (background_executor) で埋め込んで保存
// とする。議題本文は永続化しない (query_text は常に空文字。旧行も初回に消す)。
// 声 (文書) 側のベクトルは scripts/build-voice-embeddings.ts が offline で embeddings テーブルに構築する。
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/sqlite.hpp"
#include "core/vectors/embed_retry.hpp"
#include "core/vectors/embedder.hpp"

namespace di::core::vectors {

/** キャッシュ行数の上限 (超過時は古い順に削除)。 */
inline constexpr std::size_t kQueryCacheMaxRows = 1000;
/** プロセス内キャッシュの件数上限 (移植で追加。超えたら空にして DB から引き直す)。 */
inline constexpr std::size_t kQueryCacheMaxMemoryEntries = 4096;

/** sha256("<model>\n<text>") の hex。モデルが変わればベクトル空間が変わるのでモデル名をキーに含める。 */
std::string hash_query(const std::string& model, const std::string& text);

void ensure_query_cache_table(sql::Database& raw);

/** 正規化: 語順・重複・空白揺れでキャッシュキーが割れないようにする (trim → 小文字 → 重複除去 → 並べ替え)。 */
std::string normalize_query_text(const std::vector<std::string>& terms);

/** キャッシュ済みクエリベクトルを同期で引く。無ければ nullopt。 */
std::optional<std::vector<double>> get_cached_query_vector(sql::Database& raw, const std::string& model, const std::string& query_text);

/**
 * クエリを埋め込んでキャッシュに保存する (呼んだスレッドで行う。同じクエリの同時実行は 1 本にまとめる)。
 * 失敗は投げずに warn に流す。呼び出し元が DB を閉じていたら同じパスを短時間だけ開き直して保存する。
 */
void warm_query_embedding_sync(const sql::DatabasePtr& raw, const std::shared_ptr<EmbeddingClient>& embedder,
                               const std::string& model, const std::string& query_text, const WarnFn& warn);

/**
 * warm_query_embedding_sync を background_executor で行う (TS の非同期 warm)。完了を待てる future を返す。
 * キューが満杯なら warn して何もしない (キーワード検索で継続)。
 */
std::shared_future<void> warm_query_embedding(const sql::DatabasePtr& raw, const std::shared_ptr<EmbeddingClient>& embedder,
                                              const std::string& model, const std::string& query_text, const WarnFn& warn);

/** テスト用: プロセス内キャッシュを空にする。 */
void reset_query_embed_cache_for_test();

} // namespace di::core::vectors
