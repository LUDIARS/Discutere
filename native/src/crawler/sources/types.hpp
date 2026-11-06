#pragma once
// External discussion sources — 取り込み中間データモデル (src/crawler/sources/types.ts の移植)。
//
// 外部の公開ゲーム議論 (Steam レビュー / YouTube コメント等) を収集し、
// Discatier Core の utterances / reactions に格納するための正規化済み 1 発話。
// 仕様は spec/feature/crawler/EXTERNAL-SOURCES.md。
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "base/json.hpp"
#include "infra/http_client.hpp"

namespace di::crawler::sources {

// ExternalSource ("steam" | "youtube" | "youtube-livechat" | "website" | "reddit" | "niconico" | "fandom"
// | "opencritic" | "feedback" | "glab")。
// feedback: 内部のゲーム感想チャンネル / 学習フロー収集の匿名意見 (外部クロールではない)。
// glab: GLAB で集めた「遊んだ感想」(Voluptas の書き出し口)。書き手の情報は Voluptas 側で落としてある。
using ExternalSource = std::string;
bool is_external_source(std::string_view s);

/** 賛否 / 人気度 (取得元にあれば) */
struct ExternalSignal {
    /** Steam recommended 等。賛否が二値で取れる source のみ */
    std::optional<bool> votedUp;
    /** いいね / upvote / helpful 数 */
    std::optional<double> upvotes;
};

/** collector の出力かつ JSONL stage の 1 行 */
struct ExternalUtterance {
    ExternalSource source;
    /** 取得元での一意 id (dedup key の素) */
    std::string nativeId;
    /** どのゲームの議論か (Game.id と揃える slug) */
    std::string gameSlug;
    /** 議論の場の単位 → session_id に対応 (appid / 動画id / スレッドid) */
    std::string threadKey;
    /** 発話本文 (原文 = raw)。 */
    std::string content;
    /** 要約 (任意)。長文 source 用の 2 層化。 */
    std::optional<std::string> summary;
    /** "ja" | "en" 等 (判明すれば) */
    std::optional<std::string> lang;
    /** epoch ms (JS の Date.parse が NaN を返す入力は NaN。JSON では null)。 */
    double postedAt = 0;
    /** 0-based elapsed milliseconds within a source video, when the source provides it. */
    std::optional<double> videoOffsetMs;
    /** 公開・安定な同一性 ID (SteamID64 / channelId / username 等)。 */
    std::string authorId;
    /** 公開表示名 (persona の手触り。任意。逆引き用途には使わない) */
    std::optional<std::string> authorName;
    /** 返信元 nativeId (スレッド構造) → responds_to に対応 */
    std::optional<std::string> parentNativeId;
    std::optional<ExternalSignal> signal;
    /** canonical な引用元 (監査用) */
    std::string sourceUrl;
};

Json to_json(const ExternalSignal& v);
Json to_json(const ExternalUtterance& v);
/** JSONL stage の 1 行を読む (必須キー欠落は nullopt)。 */
std::optional<ExternalUtterance> utterance_from_json(const Json& j);

/** fetch の差し替え口 (TS の fetchImpl)。既定は http::fetch。 */
using FetchFn = std::function<http::ClientResponse(const http::ClientRequest&)>;
/** fetch_impl が空なら http::fetch を呼ぶ。 */
http::ClientResponse do_fetch(const FetchFn& fetch_impl, const http::ClientRequest& request);

/** JS の Date.parse 相当 (ISO 8601)。読めなければ NaN。 */
double js_date_parse(std::string_view text);

/** crawler の外部取得の同時実行の上限 (プロセス全体で 4 本。待てなければ失敗)。 */
inline constexpr std::size_t kCrawlerMaxParallel = 4;

/** 遅延 (politeDelay)。0 以下は待たない。 */
void polite_sleep(long long ms);

} // namespace di::crawler::sources
