#pragma once
// Voluptas が届ける「遊んだ感想」の受け取り (src/persona-bridge/impression-intake.ts の移植、
// spec/feature/flow/voluptas-impression-push.md)。
//
// Voluptas には匿名性を担保したうえで Discutere に情報を提供する義務がある。受け手でも次を確かめる:
//   - 1 件が持てるのは id / gameTitle / recommend / polarity / comment / createdAt だけ。
//     それ以外 (userId / authorId / name 等) を含む感想は 1 件ごと拒否して数だけ返す
//   - 書き手は常に匿名 (GLAB_ANONYMOUS_AUTHOR) で保存する (map_impression)
// 取込は既存の importer (dedup あり) なので、同じ感想を何度送られても二重にならない。
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#include "base/json.hpp"
#include "crawler/sources/types.hpp"
#include "flow/user_voices/voluptas_impressions.hpp"

namespace di::persona_bridge {

/** 1 回の送信で受ける最大件数。 */
inline constexpr std::size_t IMPRESSION_PUSH_MAX = 200;
/** 1 件の id (/^[\w.:-]{1,128}$/)・gameTitle (200)・comment (10,000) の上限 (UTF-16 単位)。 */
inline constexpr std::size_t kImpressionIdMax = 128;
inline constexpr std::size_t kImpressionGameTitleMax = 200;
inline constexpr std::size_t kImpressionCommentMax = 10'000;
/** 送信本文の JSON を読む上限 (移植で追加。200 件 × 1 万字の感想が入る大きさ)。 */
inline constexpr std::size_t kImpressionBodyMaxBytes = 8u << 20;
inline constexpr JsonLimits kImpressionBodyJsonLimits{kImpressionBodyMaxBytes, 10'000, 8};

struct ImpressionSkipped {
    int non_anonymous_field = 0;
    int invalid = 0;
};
Json to_json(const ImpressionSkipped& s);

struct ParsedImpressionBatch {
    std::vector<flow::user_voices::VoluptasImpression> impressions;
    ImpressionSkipped skipped;
};

class ImpressionBatchError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/** 匿名の感想が持てる項目だけか。 */
bool is_anonymous_impression(const Json& o);
/** 形の検査 (id / gameTitle / comment / createdAt / recommend / polarity)。 */
bool is_valid_impression(const Json& o);

/** 送信本文 `{ impressions: [...] }` を検査する。形が壊れていれば ImpressionBatchError。 */
ParsedImpressionBatch parse_impression_batch(const Json& body);

struct IngestDeps {
    /** 既存 importer (dedup 付き)。@returns 新しく取り込んだ件数。 */
    std::function<int(const std::vector<crawler::sources::ExternalUtterance>& items)> importItems;
    /** 埋め込み (未設定なら空)。@returns 書き込んだ件数。 */
    std::function<int(const std::vector<std::string>& utterance_ids)> embed;
    std::function<void(const std::string&)> warn;
};

struct IngestResult {
    int received = 0;
    int imported = 0;
    int embedded = 0;
    /** ゲームごとの件数 (gameSlug → 件数、出現順)。 */
    Json games = Json::object();
};
Json to_json(const IngestResult& r);

/** 感想を KG に取り込む (ゲーム単位の slug を付ける)。 */
IngestResult ingest_impressions(const std::vector<flow::user_voices::VoluptasImpression>& impressions, const IngestDeps& deps);

} // namespace di::persona_bridge
