// src/queue/snapshot.ts の移植テスト (golden: native/tests/golden/w1-A-queue.gen.ts)。
#include "../check.hpp"
#include "base/fsutil.hpp"
#include "base/strings.hpp"
#include "queue/snapshot.hpp"

using namespace di;

TEST_CASE("golden: buildQueueSnapshot / formatQueueText") {
    auto text = fs::read_file(test::fixtures_dir() + "/queue/snapshot.json");
    CHECK(text.has_value());
    const Json fx = parse_json_or_throw(*text, JsonLimits{8u << 20, 1'000'000, 64});
    for (const auto& c : fx["cases"]) {
        auto core = sql::Database::open(":memory:");
        for (const auto& s : fx["coreSql"]) core->exec(s.get<std::string>());
        sql::DatabasePtr pe;
        if (c["withPe"].get<bool>()) {
            pe = sql::Database::open(":memory:");
            for (const auto& s : fx["peSql"]) pe->exec(s.get<std::string>());
        }
        const auto snap = queue::build_queue_snapshot(*core, pe.get(), c["workspace"].get<std::string>(), 0);
        CHECK_EQ(json_stringify(queue::to_json(snap)), json_stringify(c["snapshot"]));
        CHECK_EQ(queue::format_queue_text(snap), c["text"].get<std::string>());
    }
}

TEST_CASE("境界: まとめ文の 1900 字切り詰め") {
    queue::QueueSnapshot snap;
    snap.workspaceId = std::string(1950, 'w');
    const auto t = queue::format_queue_text(snap);
    CHECK_EQ(str::js_length(t), std::size_t{1900});
    CHECK(str::ends_with(t, "…"));
}

DI_TEST_MAIN()
