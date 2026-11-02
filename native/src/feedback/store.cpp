#include "feedback/store.hpp"

#include "base/strings.hpp"

namespace di::feedback {

Json to_json(const GameFeedback& f) {
    Json o = Json::object();
    o["id"] = f.id;
    o["gameTitle"] = f.gameTitle;
    o["content"] = f.content;
    o["createdAt"] = f.createdAt;
    return o;
}

Json to_json(const GameCount& g) {
    Json o = Json::object();
    o["gameTitle"] = g.gameTitle;
    o["count"] = g.count;
    return o;
}

GameFeedbackStore::GameFeedbackStore(sql::DatabasePtr db) : db_(std::move(db)) {
    db_->exec(R"(
      CREATE TABLE IF NOT EXISTS game_feedback (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        game_title TEXT NOT NULL,
        content TEXT NOT NULL,
        created_at INTEGER NOT NULL
      );
      CREATE INDEX IF NOT EXISTS idx_game_feedback_title ON game_feedback (game_title, created_at);
    )");
}

std::optional<std::int64_t> GameFeedbackStore::add(const std::string& game_title, const std::string& content,
                                                   std::int64_t created_at) {
    const std::string title = str::trim(game_title);
    const std::string body = str::trim(content);
    if (title.empty() || body.empty()) return std::nullopt;
    const auto info = db_->run("INSERT INTO game_feedback (game_title, content, created_at) VALUES (?, ?, ?)", {title, body, created_at});
    return info.last_insert_rowid;
}

std::vector<GameFeedback> GameFeedbackStore::recent_for_game(const std::string& game_title, std::int64_t limit) {
    std::vector<GameFeedback> out;
    for (const auto& r : db_->all("SELECT id, game_title, content, created_at FROM game_feedback WHERE game_title = ? ORDER BY created_at DESC LIMIT ?",
                                  {str::trim(game_title), limit}))
        out.push_back({r.i64("id"), r.str("game_title"), r.str("content"), r.i64("created_at")});
    return out;
}

std::int64_t GameFeedbackStore::count_for_game(const std::string& game_title) {
    auto row = db_->get("SELECT COUNT(*) AS c FROM game_feedback WHERE game_title = ?", {str::trim(game_title)});
    return row ? row->i64("c") : 0;
}

std::vector<GameCount> GameFeedbackStore::list_games() {
    std::vector<GameCount> out;
    for (const auto& r : db_->all("SELECT game_title, COUNT(*) AS c FROM game_feedback GROUP BY game_title ORDER BY c DESC"))
        out.push_back({r.str("game_title"), r.i64("c")});
    return out;
}

} // namespace di::feedback
