// src/external-discussion/{contracts,participation,store}.ts の移植テスト。
// TS: tests/flow/external-discussion.test.ts (store / opportunity / parseConversation の部分)
// golden: native/tests/golden/w1-A-external_discussion.gen.ts
#include "../check.hpp"
#include "base/fsutil.hpp"
#include "external_discussion/contracts.hpp"
#include "external_discussion/participation.hpp"
#include "external_discussion/store.hpp"

using namespace di;
using namespace di::external_discussion;

namespace {
Json fixture() {
    auto text = fs::read_file(test::fixtures_dir() + "/external_discussion/contracts.json");
    if (!text) throw std::runtime_error("fixture missing");
    return parse_json_or_throw(*text, JsonLimits{8u << 20, 1'000'000, 64});
}
Json opp_json(const std::optional<Opportunity>& o) {
    if (!o) return Json(nullptr);
    return Json{{"id", o->id}, {"humanId", o->humanId}, {"humanAt", o->humanAt}, {"mode", o->mode}};
}
}  // namespace

TEST_CASE("golden: parseConversation (検査・並べ替え・境界)") {
    const Json fx = fixture();
    CHECK(fx["parse"].size() >= 20);
    for (const auto& c : fx["parse"]) {
        try {
            const auto out = parse_conversation(c["input"]);
            CHECK(c["ok"].get<bool>());
            CHECK_EQ(json_stringify(to_json(out)), json_stringify(c["output"]));
        } catch (const ConversationError& e) {
            if (c["ok"].get<bool>()) std::cerr << "unexpected: " << e.what() << "\n";
            CHECK(!c["ok"].get<bool>());
            CHECK_EQ(std::string(e.what()), c["error"].get<std::string>());
        }
    }
}

TEST_CASE("golden: opportunity") {
    const Json fx = fixture();
    for (const auto& c : fx["opportunities"]) {
        // opportunity は検査前の値も受けるので、検査せずに写す
        ConversationInput input;
        input.scene = c["input"]["scene"].get<std::string>();
        input.enabled = c["input"]["enabled"].get<bool>();
        for (const auto& m : c["input"]["messages"])
            input.messages.push_back({m["id"].get<std::string>(), m["kind"].get<std::string>(), m["text"].get<std::string>(),
                                      m["at"].get<std::int64_t>()});
        CHECK_EQ(json_stringify(opp_json(opportunity(input, c["now"].get<std::int64_t>()))), json_stringify(c["output"]));
    }
}

TEST_CASE("store: claim / finish / replay / busy / waiting / lease lost") {
    auto db = sql::Database::open(":memory:");
    ParticipationStore store(db);
    const std::int64_t now = 1'000'000;
    auto c1 = store.claim("actio:a", "op1", "h1", now);
    CHECK(c1.token.has_value());
    // 同じ機会はリース中 busy、同じ scene の別機会も busy
    CHECK_EQ(store.claim("actio:a", "op1", "h1", now).result->status, std::string("busy"));
    CHECK_EQ(store.claim("actio:a", "op2", "h2", now).result->status, std::string("busy"));
    CHECK_THROWS(store.finish("op1", "wrong-token", ParticipationResult{"skipped", {}, {}, {}, {}}));
    ParticipationResult proposal{"proposal", std::string("op1"), std::string("発言"), std::string("pro"), std::string("h1")};
    store.finish("op1", *c1.token, proposal);
    // 結果の再生 (同じ機会は同じ結果を返す)
    CHECK(store.claim("actio:a", "op1", "h1", now + 1).result == proposal);
    // 同じ人間の発言にはもう答えない
    CHECK_EQ(store.claim("actio:a", "op3", "h1", now + 1).result->status, std::string("skipped"));
    // 直近 3 分以内に提案した scene は waiting
    CHECK_EQ(store.claim("actio:a", "op4", "h4", now + 1).result->status, std::string("waiting"));
    auto later = store.claim("actio:a", "op5", "h5", now + 180'001);
    CHECK(later.token.has_value());
    store.fail("op5", *later.token);
    // 失敗した試行はリースを手放すので取り直せる
    CHECK(store.claim("actio:a", "op5", "h5", now + 180'002).token.has_value());
    // 壊れた保存結果は例外 (TS の JSON.parse と同じ)
    db->run("INSERT INTO external_discussion_attempts VALUES ('bad','actio:b','done','{broken','h','o',0,0)");
    CHECK_THROWS(store.claim("actio:b", "bad", "h", now));
}

TEST_CASE("locale_compare は ICU の ASCII 順") {
    CHECK(locale_compare("a", "A") < 0);
    CHECK(locale_compare("a-b", "ab") < 0);
    CHECK(locale_compare("ab", "aB") < 0);
    CHECK(locale_compare("aB", "Ab") < 0);
    CHECK(locale_compare("a10", "a2") < 0);
    CHECK_EQ(locale_compare(std::string("x\0", 2), "x"), 0);
}

DI_TEST_MAIN()
