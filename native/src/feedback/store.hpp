#pragma once
// ゲーム感想の匿名収集ストア。移植元 src/feedback/store.ts。
//
// Discord の「ゲーム感想」カテゴリ配下のチャンネル (チャンネル名 = ゲームタイトル) に
// 投稿された感想を、議論にせず意見データとして蓄積する。
// 個人データ規約 (CLAUDE.md): 投稿者名・アカウント名は保存しない (匿名)。
// 保存するのは gameTitle / 本文 / 時刻 のみ。spec/feature/game-feedback.md。
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "base/sqlite.hpp"

namespace di::feedback {

struct GameFeedback {
    std::int64_t id = 0;
    std::string gameTitle;
    std::string content;
    std::int64_t createdAt = 0;
};

struct GameCount {
    std::string gameTitle;
    std::int64_t count = 0;
};

Json to_json(const GameFeedback& f);
Json to_json(const GameCount& g);

class GameFeedbackStore {
public:
    explicit GameFeedbackStore(sql::DatabasePtr db);
    /** 感想を 1 件追加 (投稿者は保存しない)。空本文・空タイトルは無視して nullopt。戻り値は id。 */
    std::optional<std::int64_t> add(const std::string& game_title, const std::string& content, std::int64_t created_at);
    /** ゲームの直近感想を新しい順で返す。 */
    std::vector<GameFeedback> recent_for_game(const std::string& game_title, std::int64_t limit = 12);
    std::int64_t count_for_game(const std::string& game_title);
    /** 収集済みゲームタイトル一覧 (件数降順)。 */
    std::vector<GameCount> list_games();

private:
    sql::DatabasePtr db_;
};

} // namespace di::feedback
