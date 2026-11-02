#pragma once
// 外部の声検索コア — キーワード recall + ベクトル rerank のハイブリッド (src/discatier-engine-adapter/voice-search.ts)。
//
// 検索の 3 段構え (spec/feature/voice-rag-hybrid.md):
//   1. recall: 議題語を分解 (extract_key_terms) → ゲーム名/用語の別名展開
//      (build_alias_groups + Ludus 中央辞書) → SQL LIKE で候補集め (全件照合。直近 N 件制限は無い)。
//   2. rerank: config.embedding.enabled かつクエリ埋め込みがキャッシュ済みなら、
//      候補のベクトル (embeddings テーブル、offline 構築) と cosine を取り、キーワード順位と RRF 融合する。
//      未キャッシュ時はキーワード順のまま返し、裏 (background_executor) でクエリ埋め込みを温める
//      (次回呼び出しからハイブリッド化。キューが満杯なら温めない)。候補のベクトルが一部しか無い間も、
//      索引有無の偏りを避けてキーワード順へ degrade する。
//   3. diversify: 近似重複を畳み (bigram Jaccard)、MMR で「関連度 × 多様性」の上位 limit 件を選ぶ。
//
// 同期 interface を壊さないため、非同期が必要なのはクエリ埋め込みの温めだけに隔離してある (query_embed_cache)。
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/json.hpp"
#include "core/index.hpp"
#include "core/vectors/embedder.hpp"

namespace di::discatier_engine_adapter {

/** 関連語が無い時に opinion で拾う直近件数。 */
inline constexpr int RECENT_CAP = 300;
/** LIKE 一致候補の上限 (早期打ち切りで全件 LIKE でも軽い)。 */
inline constexpr int KEYWORD_CAP = 2000;
/** 別名展開後の検索語上限 (LIKE 句の暴発防止。用語辞書分をやや広めに取る)。 */
inline constexpr std::size_t EXPANDED_TERMS_MAX = 16;
/** ベクトル rerank に回す候補上限 (vector_json の parse コスト上限)。 */
inline constexpr std::size_t RERANK_CAP = 1000;
/** MMR の関連度配分 (0.7 = 関連度寄り)。 */
inline constexpr double MMR_LAMBDA = 0.7;

struct ScoredVoiceRow {
    std::string id;
    std::optional<std::string> speakerId;
    std::string rawContent;
    /** キーワード合成スコア (hits*10 + opinion)。順序はハイブリッド融合順が優先。 */
    double score = 0;
};
Json to_json(const ScoredVoiceRow& r);

/** 議題語 → 別名展開済み検索語 (ゲーム名 + Ludus 用語辞書)。 */
std::vector<std::string> build_search_terms(core::Core& core, const std::string& workspace_id, const std::vector<std::string>& terms);

struct SearchOptions {
    /** false でベクトル rerank を使わない (既定 true = 設定に従う)。 */
    std::optional<bool> useEmbedding;
};

using VoiceWarnFn = std::function<void(const std::string&)>;

/**
 * 外部の声を検索し、融合順に上位 limit 件を返す。
 * 同期関数 — ベクトル rerank はクエリ埋め込みがキャッシュ済みの時だけ効く。
 * warn 省略時は "[voice-search] <msg>" を log::warn。
 * @implements SPEC-VOICE-RAG-HYBRID-SEARCH
 */
std::vector<ScoredVoiceRow> search_external_voice_rows(core::Core& core, const std::string& workspace_id, const std::vector<std::string>& terms,
                                                       int limit, const VoiceWarnFn& warn = {}, const SearchOptions& options = {});

/** テスト用: 温めに使う埋め込みクライアントを差し替える (nullptr で設定から作る既定に戻す)。 */
void set_voice_embedder_for_test(std::shared_ptr<core::vectors::EmbeddingClient> embedder);

} // namespace di::discatier_engine_adapter
