// w2-J: src/db/repository.ts (drizzle クエリ) の生 SQL 版。既定値・timestamp の秒丸め・部分更新・ON CONFLICT DO NOTHING・並び順。
#include "check.hpp"
#include "core/test_util.hpp"

#include "db/repository.hpp"

using namespace di;
using namespace di::db;

namespace {
sql::DatabasePtr open_db(const test::TempDir& dir) {
    auto db = sql::Database::open(dir.file("main.db"));
    ensure_schema(*db);
    return db;
}
} // namespace

TEST_CASE("users: upsert は insert / update を切り替える") {
    test::TempDir dir;
    auto db = open_db(dir);
    UserRepo users(db);
    users.upsert({"u1", "login", "Name", std::nullopt, "", "general"});
    auto u = users.find_by_id("u1");
    CHECK(u && u->displayName == "Name" && !u->email && u->createdAt > 1'700'000'000 && u->createdAt < 10'000'000'000);
    users.upsert({"u1", "login", "New", std::string("a@example.test"), "", "admin"});
    u = users.find_by_id("u1");
    CHECK(u && u->displayName == "New" && u->email == std::string("a@example.test") && u->role == "admin");
    CHECK(!users.find_by_id("none"));
}

TEST_CASE("ludus mechanics / learning jobs / economy graph") {
    test::TempDir dir;
    auto db = open_db(dir);
    LudusMechanicRepo mech(db);
    mech.create(Json{{"id", "m1"}, {"workspaceId", "ws"}, {"gameTitle", "G"}, {"mechanic", "ガチャ"}, {"sourceUrl", "https://s/1"}, {"confidence", 50},
                     {"createdAt", 1'000'000}});
    // unique(workspace, sourceUrl, mechanic) は ON CONFLICT DO NOTHING
    mech.create(Json{{"id", "m2"}, {"workspaceId", "ws"}, {"gameTitle", "G"}, {"mechanic", "ガチャ"}, {"sourceUrl", "https://s/1"}});
    mech.create(Json{{"id", "m3"}, {"workspaceId", "ws"}, {"gameTitle", "G"}, {"mechanic", "協力"}, {"sourceUrl", "https://s/2"}, {"unknownKey", 1}});
    const auto by_game = mech.search_by_game("ws", "G");
    CHECK_EQ(by_game.size(), std::size_t{2});
    CHECK_EQ(by_game[0].id, std::string("m1"));  // confidence 降順
    CHECK_EQ(by_game[0].createdAt, 1000);         // ミリ秒 → 秒
    CHECK_EQ(by_game[1].confidence, 0);           // .default(0)
    CHECK_EQ(mech.list_by_workspace("ws", 1).size(), std::size_t{1});
    CHECK_THROWS(mech.create(Json{{"id", "m4"}, {"workspaceId", "ws"}, {"gameTitle", Json::array()}, {"mechanic", "x"}, {"sourceUrl", "u"}}));
    CHECK_THROWS(mech.create(Json::array()));

    LudusLearningJobRepo jobs(db);
    jobs.create(Json{{"id", "j1"}, {"workspaceId", "ws"}, {"gameTitle", "G"}, {"query", "q"}});
    auto j = jobs.find_by_id("j1");
    CHECK(j && j->status == "queued" && j->fetchedCount == 0 && !j->error);
    jobs.update("j1", Json{{"status", "failed"}, {"error", "boom"}});
    j = jobs.find_by_id("j1");
    CHECK(j && j->status == "failed" && j->error == std::string("boom") && j->query == "q");
    jobs.update("j1", Json{{"error", nullptr}});
    CHECK(!jobs.find_by_id("j1")->error);
    CHECK_EQ(jobs.list_by_workspace("ws").size(), std::size_t{1});

    EconomyGraphRepo eco(db);
    eco.upsert(Json{{"id", "e1"}, {"workspaceId", "ws"}, {"gameTitle", "G"}, {"graphJson", "{}"}, {"mechanicCount", 2}});
    eco.upsert(Json{{"id", "e2"}, {"workspaceId", "ws"}, {"gameTitle", "G"}, {"graphJson", "{\"a\":1}"}, {"mechanicCount", 5}});
    const auto list = eco.list_by_workspace("ws");
    CHECK(list.size() == 1 && list[0].id == "e1" && list[0].mechanicCount == 5 && list[0].graphJson == "{\"a\":1}");
}

TEST_CASE("tasks / logs / monitors / chat") {
    test::TempDir dir;
    auto db = open_db(dir);
    TaskRepo tasks(db);
    tasks.create(Json{{"id", "t1"}, {"workspaceId", "ws"}, {"title", "a"}, {"createdBy", "x"}, {"assigneeId", "p"}, {"isCriticalPath", true}});
    tasks.create(Json{{"id", "t2"}, {"workspaceId", "ws"}, {"title", "b"}, {"createdBy", "x"}, {"status", "done"}});
    CHECK(tasks.find_by_id("t1")->isCriticalPath);
    CHECK_EQ(tasks.find_by_workspace_id_and_status("ws", "pending").size(), std::size_t{1});
    CHECK_EQ(tasks.find_by_assignee("p").size(), std::size_t{1});
    tasks.update("t1", Json{{"title", "a2"}});
    CHECK_EQ(tasks.find_by_id("t1")->title, std::string("a2"));
    TaskLogRepo logs(db);
    logs.create(Json{{"id", "l1"}, {"taskId", "t1"}, {"action", "create"}, {"performedBy", "x"}});
    CHECK_EQ(logs.find_by_task_id("t1").size(), std::size_t{1});
    tasks.delete_by_id("t2");
    CHECK_EQ(tasks.find_by_workspace_id("ws").size(), std::size_t{1});

    MonitorRepo monitors(db);
    monitors.create(Json{{"id", "mo1"}, {"workspaceId", "ws"}, {"platform", "discord"}, {"channelId", "c"}, {"channelName", "n"}, {"createdBy", "x"}});
    auto m = monitors.find_by_id("mo1");
    CHECK(m && m->isActive && m->captureMessages && m->mode == "task" && m->discussionDelayMinutes == 5);
    monitors.update("mo1", Json{{"isActive", false}});
    CHECK(monitors.find_active_by_workspace_id("ws").empty());
    CHECK_EQ(monitors.find_by_workspace_id("ws").size(), std::size_t{1});

    ChatMessageRepo chat(db);
    for (int i = 0; i < 3; ++i)
        chat.create(Json{{"id", "c" + std::to_string(i)}, {"monitorId", "mo1"}, {"workspaceId", "ws"}, {"platform", "discord"}, {"channelId", "c"},
                         {"messageId", "msg" + std::to_string(i)}, {"authorId", "a"}, {"authorName", "A"}, {"text", "t"}, {"postedAt", 1000 + i}});
    chat.create(Json{{"id", "dup"}, {"monitorId", "mo1"}, {"workspaceId", "ws"}, {"platform", "discord"}, {"channelId", "c"},
                     {"messageId", "msg0"}, {"authorId", "a"}, {"authorName", "A"}, {"text", "t"}, {"postedAt", 1}});
    CHECK_EQ(chat.count_by_monitor_id("mo1"), 3);
    CHECK(chat.exists_by_message_id("mo1", "msg1") && !chat.exists_by_message_id("mo1", "zzz"));
    const auto latest = chat.find_by_monitor_id("mo1", {2, std::nullopt});
    CHECK(latest.size() == 2 && latest[0].postedAt == 1002);
    CHECK_EQ(chat.find_by_monitor_id("mo1", {std::nullopt, 1001}).size(), std::size_t{2});
    const auto range = chat.find_by_monitor_id_in_range("mo1", 1001, 1002);
    CHECK(range.size() == 2 && range[0].postedAt == 1001);
    ChatSummaryRepo sums(db);
    sums.create(Json{{"id", "s1"}, {"monitorId", "mo1"}, {"workspaceId", "ws"}, {"periodStart", 1}, {"periodEnd", 2}, {"summary", "x"}, {"createdBy", "b"}});
    CHECK(sums.find_by_id("s1") && sums.find_by_monitor_id("mo1").size() == 1);
    sums.delete_by_id("s1");
    CHECK(!sums.find_by_id("s1"));
    monitors.delete_by_id("mo1");
}

DI_TEST_MAIN()
