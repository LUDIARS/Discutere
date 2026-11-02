// w3-O: 軽量 Web チャットの部品 (src/web-chat/session.ts / transcript.ts)。
// golden: room id の正規化と room の会話読み出し (w3o/golden.json の rooms / transcript)。
// 個人データを保存しないこと (author は表示名のみ・人間発話のミラー重複を出さない) と、session の確保・議論の有無を確かめる。
#include "w3o/w3o_support.hpp"

#include "base/strings.hpp"
#include "core/index.hpp"
#include "web_chat/session.hpp"
#include "web_chat/transcript.hpp"

using namespace di;
using namespace di::web_chat;
using w3o::golden;

TEST_CASE("golden: sanitize_room_id (英数 . _ - のみ・32 文字・空は lobby)") {
    for (const auto& c : golden()["rooms"]) {
        const std::string raw = c["raw"].is_null() ? std::string() : c["raw"].get<std::string>();
        CHECK_EQ(sanitize_room_id(raw), c["id"].get<std::string>());
    }
    CHECK_EQ(web_scene("r1"), std::string("web:r1"));
}

TEST_CASE("golden: read_room_transcript (入口の人間 + 議論の非人間・since/limit・名前解決)") {
    const auto& g = golden()["transcript"];
    auto db = sql::Database::open(":memory:");
    db->exec(R"(CREATE TABLE sessions (id TEXT PRIMARY KEY, workspace_id TEXT, title TEXT, scene TEXT, started_at INTEGER);
                CREATE TABLE utterances (id TEXT PRIMARY KEY, session_id TEXT, speaker_id TEXT, raw_content TEXT, posted_at INTEGER);)");
    for (const auto& s : g["sessions"])
        db->run("INSERT INTO sessions VALUES (?, ?, ?, ?, ?)", {s[0].get<std::string>(), s[1].get<std::string>(), s[2].get<std::string>(),
                                                                 s[3].get<std::string>(), s[4].get<std::int64_t>()});
    for (const auto& u : g["utterances"])
        db->run("INSERT INTO utterances VALUES (?, ?, ?, ?, ?)",
                {u[0].get<std::string>(), u[1].get<std::string>(), u[2].is_null() ? sql::Value() : sql::Value(u[2].get<std::string>()),
                 u[3].get<std::string>(), u[4].get<std::int64_t>()});
    for (const auto& c : g["cases"]) {
        const auto& o = c["options"];
        ReadTranscriptOptions opts;
        if (o.contains("sinceTs")) opts.sinceTs = o["sinceTs"].get<double>();
        if (o.contains("limit")) opts.limit = o["limit"].get<std::int64_t>();
        if (o.contains("resolve")) opts.resolveName = [](const std::string& id) { return "名:" + id; };
        const std::string room = o.contains("room") ? o["room"].get<std::string>() : "r1";
        Json out = Json::array();
        for (const auto& m : read_room_transcript(*db, "knowledge", room, opts)) out.push_back(to_json(m));
        W3O_SAME(out, c["result"]);
    }
}

TEST_CASE("ensure_web_session は room ごとに 1 つ・web_discussion_exists は open な議論だけ") {
    w3o::TempDir dir;
    auto core = core::create_core(dir.path + "/kg.sqlite");
    const auto a = ensure_web_session(*core, "knowledge", "r1");
    CHECK_EQ(ensure_web_session(*core, "knowledge", "r1"), a);
    CHECK(ensure_web_session(*core, "knowledge", "r2") != a);
    const auto row = core->raw().get("SELECT title, scene FROM sessions WHERE id = ?", {a});
    CHECK_EQ(row->str("title"), std::string("web-session:r1"));
    CHECK_EQ(row->str("scene"), std::string("web:r1"));

    CHECK(!web_discussion_exists(*core, "knowledge", "r1"));
    core->raw().run("INSERT INTO design_gaps (id, workspace_id, title, status, created_at, updated_at) VALUES ('g1', 'knowledge', 'x', 'closed', 1, 1)");
    core->raw().run("INSERT INTO sessions (id, workspace_id, title, started_at, created_at, updated_at, scene) VALUES ('d1', 'knowledge', 'discussion-of-gap:g1', 1, 1, 1, 'web:r1')");
    CHECK(!web_discussion_exists(*core, "knowledge", "r1"));
    core->raw().run("UPDATE design_gaps SET status = NULL WHERE id = 'g1'");
    CHECK(web_discussion_exists(*core, "knowledge", "r1"));
    CHECK(!web_discussion_exists(*core, "other", "r1"));
    CHECK(!web_discussion_exists(*core, "knowledge", "r2"));
    core->close();
}

TEST_CASE("境界: 巨大な limit / 巨大な room id でも上限つきで読む") {
    auto db = sql::Database::open(":memory:");
    db->exec(R"(CREATE TABLE sessions (id TEXT PRIMARY KEY, workspace_id TEXT, title TEXT, scene TEXT);
                CREATE TABLE utterances (id TEXT PRIMARY KEY, session_id TEXT, speaker_id TEXT, raw_content TEXT, posted_at INTEGER);)");
    db->run("INSERT INTO sessions VALUES ('s', 'knowledge', 'web-session:r', 'web:r')");
    for (int i = 0; i < 50; ++i) db->run("INSERT INTO utterances VALUES (?, 's', '人', 'x', ?)", {"u" + std::to_string(i), i});
    ReadTranscriptOptions opts;
    opts.limit = 1'000'000'000;
    CHECK_EQ(read_room_transcript(*db, "knowledge", "r", opts).size(), 50u);
    opts.limit = 0;
    CHECK_EQ(read_room_transcript(*db, "knowledge", "r", opts).size(), 0u);
    CHECK_EQ(sanitize_room_id(std::string(1u << 20, 'a')), std::string(32, 'a'));
    CHECK(read_room_transcript(*db, "knowledge", std::string(100'000, 'r')).empty());
}

DI_TEST_MAIN()
