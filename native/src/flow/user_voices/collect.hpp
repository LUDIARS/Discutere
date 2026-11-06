#pragma once
// ユーザーの声の収集 (src/flow/user-voices/collect.ts の移植、spec/feature/flow/user-voices.md)。
//
// ゲームごとに:
//   0. Di の収集済みデータがあればそれを使う (取得先を呼ばない)。
//   1. Steam でアプリを解決し、リリース済みならレビューを取得 → KG に取込 → ベクトル化する。
//   2. Steam で取れなければ YouTube (API キーがあれば)。
//   3. それも無ければ Voluptas の「遊んだ感想」を取得 → 取込 → ベクトル化 (届いていた感想にも fallback)。
//   4. どれも取れなければ何もしない (既存の外部の声検索に任せる)。
// 取得・取込・ベクトル化は注入境界 (テストで差し替え可)。失敗は warn して次のゲームへ進む (議論を止めない)。
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "base/json.hpp"
#include "core/index.hpp"
#include "crawler/sources/types.hpp"
#include "flow/user_voices/steam_app.hpp"

namespace di::flow::user_voices {

/** 声を集める対象ゲーム。 */
struct GameRef {
    /** 表示・Voluptas 検索に使う名前 (議題そのもののこともある)。 */
    std::string title;
    /** Steam 検索に使う語 (先頭から順に試す)。 */
    std::vector<std::string> searchTerms;
    /** Steam の候補が同じゲームか判定する文字列 (議題全文 or 指定ゲーム名)。 */
    std::string matchAgainst;
};

/** "di" | "steam" | "youtube" | "glab" | "none" */
using VoiceChannel = std::string;

struct GameVoiceReport {
    std::string title;
    VoiceChannel channel = "none";
    /** 取込に使った gameSlug (ゲーム単位の声の引き出しに使う)。none / di なら nullopt。 */
    std::optional<std::string> gameSlug;
    std::optional<std::int64_t> appId;
    std::optional<std::string> appName;
    /** Steam にあるが未リリースだった。 */
    bool unreleased = false;
    int collected = 0;
    int imported = 0;
    int embedded = 0;
    /** Voluptas 取得が例外で失敗したとき false / VoluptasUnavailable なら true (TS と同じくそれ以外は未設定)。 */
    std::optional<bool> voluptasDown;
};
/** キー順は TS の spread と同じ (title, collected, imported, embedded, [unreleased, appId, appName], channel, gameSlug, [appId, appName], [voluptasDown])。 */
Json to_json(const GameVoiceReport& r);

using ItemsFn = std::function<std::vector<crawler::sources::ExternalUtterance>(const GameRef& game, const std::string& game_slug)>;

struct CollectUserVoicesDeps {
    core::Core* core = nullptr;
    std::string workspaceId;
    std::function<int(const GameRef&)> existingCount;
    ItemsFn fetchYoutube;
    std::function<std::optional<SteamApp>(const GameRef&)> resolveSteam;
    std::function<std::vector<crawler::sources::ExternalUtterance>(const SteamApp& app, const std::string& game_slug)> fetchSteam;
    /** Voluptas 未設定なら空 (Steam に無いゲームは声を集めない)。 */
    ItemsFn fetchGlab;
    /** Voluptas が自動で届けて KG に保存済みの感想の件数 (gameSlug 単位)。 */
    std::function<int(const std::string& game_slug)> storedGlabCount;
    std::function<int(core::Core* core, const std::vector<crawler::sources::ExternalUtterance>& items, const std::string& workspace_id)> importItems;
    /** 埋め込み未設定なら空 (ベクトル化しない)。@returns 書き込んだ件数。 */
    std::function<int(core::Core* core, const std::vector<std::string>& utterance_ids)> embed;
    std::function<void(const std::string&)> log;
    std::function<void(const std::string&)> warn;
};

/** importer と同じ決定的 id (`ext:<source>:<nativeId>`)。 */
std::string utterance_id_of(const crawler::sources::ExternalUtterance& item);
/** Steam のゲーム単位 slug (appId で一意)。 */
std::string steam_game_slug(std::int64_t app_id);
/** GLAB 感想のゲーム単位 slug (NFKC → 小文字 → 空白を "-" → 先頭 48 文字)。NFKC と小文字化は近似 (nfkc.hpp)。 */
std::string glab_game_slug(const std::string& title);

/** ゲーム 1 本の声を集める。 */
GameVoiceReport collect_game_voices(const GameRef& game, const CollectUserVoicesDeps& deps);
/** 複数ゲームの声を順に集める (1 本ずつ。Steam / Voluptas へ同時アクセスしない)。 */
std::vector<GameVoiceReport> collect_user_voices(const std::vector<GameRef>& games, const CollectUserVoicesDeps& deps);
/** スレッド/画面に出す収集結果の一言。 */
std::string describe_voice_report(const GameVoiceReport& r);

} // namespace di::flow::user_voices
