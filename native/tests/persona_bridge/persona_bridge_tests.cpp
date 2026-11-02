// src/persona-bridge/{authorization-assertion,utterance-export}.ts の移植テスト。
// TS: tests/flow/persona-bridge.test.ts (readHumanUtterances・カーソル・アサーション拒否)
// golden: native/tests/golden/w1-A-persona_bridge.gen.ts
#include "../check.hpp"
#include "base/fsutil.hpp"
#include "persona_bridge/authorization_assertion.hpp"
#include "persona_bridge/utterance_export.hpp"

using namespace di;
using namespace di::persona_bridge;

namespace {
Json fixture() {
    auto text = fs::read_file(test::fixtures_dir() + "/persona_bridge/persona_bridge.json");
    if (!text) throw std::runtime_error("fixture missing");
    return parse_json_or_throw(*text, JsonLimits{8u << 20, 1'000'000, 64});
}
Json rec(const ExportableUtteranceRecord& r) { return to_json(r); }
}  // namespace

TEST_CASE("golden: verifyPersonaBridgeAssertion (受理・拒否の種類と文言)") {
    const Json fx = fixture();
    CHECK(fx["assertions"].size() >= 20);
    for (const auto& c : fx["assertions"]) {
        const std::string name = c["name"].get<std::string>();
        try {
            const auto v = verify_persona_bridge_assertion(c["compact"].get<std::string>(), c["publicKey"].get<std::string>(),
                                                           c["authorId"].get<std::string>(), c["nowMs"].get<std::int64_t>());
            if (!c["ok"].get<bool>()) std::cerr << "accepted unexpectedly: " << name << "\n";
            CHECK(c["ok"].get<bool>());
            CHECK_EQ(v.authorId, c["output"]["authorId"].get<std::string>());
            CHECK_EQ(v.expiresAt, c["output"]["expiresAt"].get<std::int64_t>());
            CHECK_EQ(v.jti, c["output"]["jti"].get<std::string>());
        } catch (const PersonaBridgeAssertionVerificationError& e) {
            if (c["ok"].get<bool>() || c["error"] != e.what()) std::cerr << name << ": " << e.what() << "\n";
            CHECK_EQ(c["errorClass"].get<std::string>(), std::string("PersonaBridgeAssertionVerificationError"));
            CHECK_EQ(std::string(e.what()), c["error"].get<std::string>());
        } catch (const PersonaBridgeAssertionConfigurationError& e) {
            if (c["ok"].get<bool>()) std::cerr << name << ": " << e.what() << "\n";
            CHECK_EQ(c["errorClass"].get<std::string>(), std::string("PersonaBridgeAssertionConfigurationError"));
            CHECK_EQ(std::string(e.what()), c["error"].get<std::string>());
        }
    }
}

TEST_CASE("golden: カーソルの encode / decode / next") {
    const Json fx = fixture();
    for (const auto& c : fx["cursors"]) {
        try {
            const auto cur = decode_utterance_cursor(c["input"].get<std::string>());
            CHECK(c["ok"].get<bool>());
            CHECK_EQ(cur.postedAt, c["output"]["postedAt"].get<std::int64_t>());
            CHECK_EQ(cur.id, c["output"]["id"].get<std::string>());
        } catch (const UtteranceCursorError& e) {
            CHECK(!c["ok"].get<bool>());
            CHECK_EQ(std::string(e.what()), c["error"].get<std::string>());
        }
    }
    for (const auto& c : fx["encodes"])
        CHECK_EQ(encode_utterance_cursor({c["input"]["postedAt"].get<std::int64_t>(), c["input"]["id"].get<std::string>()}),
                 c["output"].get<std::string>());
    for (const auto& c : fx["nexts"]) {
        std::vector<ExportableUtteranceRecord> list;
        for (const auto& u : c["input"]) list.push_back({u["id"], u["text"], u["createdAt"]});
        try {
            const auto next = next_utterance_cursor(list);
            CHECK(c["ok"].get<bool>());
            CHECK_EQ(next ? Json(*next) : Json(nullptr), c["output"]);
        } catch (const UtteranceCursorError& e) {
            CHECK(!c["ok"].get<bool>());
            CHECK_EQ(std::string(e.what()), c["error"].get<std::string>());
        }
    }
}

TEST_CASE("readHumanUtterances: Discord の本人の発言だけ・同時刻のページ送り") {
    auto db = sql::Database::open(":memory:");
    db->exec("CREATE TABLE sessions (id TEXT PRIMARY KEY, workspace_id TEXT, title TEXT, scene TEXT)");
    db->exec("CREATE TABLE utterances (id TEXT PRIMARY KEY, workspace_id TEXT, session_id TEXT, speaker_id TEXT, raw_content TEXT, posted_at INTEGER)");
    db->run("INSERT INTO sessions VALUES (?, ?, ?, ?)", {"discord", "knowledge", "discord-session:guild:channel", "discord:guild/channel"});
    db->run("INSERT INTO sessions VALUES (?, ?, ?, ?)", {"web", "knowledge", "web-session:lobby", "discord:spoofed/channel"});
    auto ins = [&](const char* id, const char* session, const char* speaker, const char* text, int at) {
        db->run("INSERT INTO utterances VALUES (?, ?, ?, ?, ?, ?)", {id, "knowledge", session, speaker, text, at});
    };
    ins("human", "discord", "123456789", "本人の発言", 1000);
    ins("web-spoof", "web", "123456789", "表示名を偽装した発言", 1000);
    ins("persona", "discord", "persona:one", "AIの発言", 1001);
    ins("external", "discord", "ext:steam:123456789", "他所の発言", 1002);
    const auto first = read_human_utterances(*db, "123456789", {0, ""}, 10);
    CHECK_EQ(first.size(), std::size_t{1});
    CHECK_EQ(json_stringify(rec(first[0])), std::string(R"({"id":"human","text":"本人の発言","createdAt":"1970-01-01T00:00:01.000Z"})"));
    for (const char* id : {"page-1", "page-2", "page-3"}) ins(id, "discord", "555555555", id, 2000);
    const auto p1 = read_human_utterances(*db, "555555555", {0, ""}, 2);
    const auto p2 = read_human_utterances(*db, "555555555", decode_utterance_cursor(*next_utterance_cursor(p1)), 2);
    CHECK_EQ(p1.size(), std::size_t{2});
    CHECK_EQ(p2.size(), std::size_t{1});
    CHECK_EQ(p2[0].id, std::string("page-3"));
}

TEST_CASE("境界: カーソル長 512 ちょうど・513・壊れた base64") {
    std::string at_limit(512, 'A');
    CHECK_THROWS(decode_utterance_cursor(at_limit));  // 長さは通るが中身が壊れている
    try {
        decode_utterance_cursor(std::string(513, 'A'));
        CHECK(false);
    } catch (const UtteranceCursorError& e) {
        CHECK_EQ(std::string(e.what()), std::string("cursor is invalid"));
    }
    CHECK_THROWS(decode_utterance_cursor("%%%"));
    CHECK_EQ(node_base64url_decode("aGk"), std::string("hi"));
    CHECK_EQ(node_base64url_decode("aGkA="), std::string("hi\0", 3));
}

DI_TEST_MAIN()
