#pragma once
// Voluptas の「遊んだ感想」書き出し口 (`GET /api/personas/impressions`) の client
// (src/flow/user-voices/voluptas-impressions.ts の移植。Voluptas spec SPEC-GLAB-IMPRESSION-EXPORT)。
//
// Steam に無いゲームのユーザーの声として使う。感想には書き手の情報が無い (Voluptas 側で落とす)
// ので、authorId は匿名の固定値にする。認証は Bearer (Cernere service token / 移行期間の固定トークン)。
// Bearer はログ・エラー文に出さない。
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "crawler/sources/types.hpp"

namespace di::flow::user_voices {

class VoluptasUnavailable : public std::runtime_error {
public:
    VoluptasUnavailable() : std::runtime_error("Voluptas（Vo）が停止中、または接続できません。") {}
};

/** 感想の書き手は特定しない (Voluptas は書き手の情報を返さない)。 */
inline constexpr const char* GLAB_ANONYMOUS_AUTHOR = "anonymous";

struct VoluptasImpression {
    std::string id;
    std::string gameTitle;
    std::optional<bool> recommend;        // boolean | null | 未指定
    std::optional<std::string> polarity;  // string | null | 未指定
    std::string comment;
    std::string createdAt;
};

/** 感想 1 件 → ExternalUtterance (純関数)。 */
crawler::sources::ExternalUtterance map_impression(const std::string& game_slug, const VoluptasImpression& impression);

/** id / gameTitle / comment が文字列で、comment が空白だけでないものだけ読む。 */
std::optional<VoluptasImpression> impression_from_json(const Json& v);

struct FetchVoluptasArgs {
    /** Voluptas の origin (例 http://localhost:3015)。https 必須 (loopback のみ http 可)。 */
    std::string baseUrl;
    std::string bearer;
    std::string game;
    std::string gameSlug;
    int limit = 50;
    crawler::sources::FetchFn fetchImpl;
};

/**
 * ゲーム名で Voluptas の感想を取得する。
 * 不正な URL・HTTPS でない非 loopback は std::invalid_argument / std::runtime_error、
 * 接続失敗・5xx は VoluptasUnavailable、その他の非 2xx は std::runtime_error。
 */
std::vector<crawler::sources::ExternalUtterance> fetch_voluptas_impressions(const FetchVoluptasArgs& args);

} // namespace di::flow::user_voices
