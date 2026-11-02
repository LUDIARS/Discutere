#pragma once
// ゲーム名 → Steam アプリの解決とリリース判定 (src/flow/user-voices/steam-app.ts の移植)。
// - ストア検索 (/api/storesearch) で候補を引き、候補名が議題に含まれる (または候補が名前を含む) ものだけ採用する。
// - appdetails の release_date.coming_soon でリリース済みかを判定する。API キー不要。
// ネットワーク I/O は fetch を差し替え可能 (テスト)。既定は di::http::fetch (期限 15 秒・応答 2MiB まで・SSRF 検査あり)。
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/json.hpp"
#include "infra/http_client.hpp"

namespace di::flow::user_voices {

struct SteamApp {
    std::int64_t appId = 0;
    std::string name;
    bool released = false;
    bool operator==(const SteamApp&) const = default;
};
Json to_json(const SteamApp& app);

/** 比較用の正規化 (NFKC・小文字・空白と記号の除去)。NFKC と小文字化は近似 (nfkc.hpp)。 */
std::string normalize_game_name(std::string_view value);

/** 候補名が議題/指定名と同じゲームを指すか (双方向の部分一致、短すぎる名前は除外)。 */
bool is_same_game(std::string_view candidate, std::string_view query);

using FetchFn = std::function<http::ClientResponse(const http::ClientRequest&)>;

struct ResolveSteamAppArgs {
    /** 検索に使う語 (先頭から順に試す)。 */
    std::vector<std::string> terms;
    /** 採用判定に使う文字列 (議題全文 or 指定ゲーム名)。 */
    std::string matchAgainst;
    /** 空なら di::http::fetch。 */
    FetchFn fetchImpl;
    int maxQueries = 3;
};

/** 応答の上限 (Steam の storesearch / appdetails は数十 KB)。 */
inline constexpr std::size_t kSteamMaxResponseBytes = 2u << 20;

/**
 * 検索語の候補から Steam アプリを 1 つ解決する。見つからなければ nullopt。
 * TS と同じく、通信そのものの失敗と壊れた JSON は例外 (std::runtime_error) で伝える。HTTP の非 2xx は「無し」扱い。
 */
std::optional<SteamApp> resolve_steam_app(const ResolveSteamAppArgs& args);

} // namespace di::flow::user_voices
