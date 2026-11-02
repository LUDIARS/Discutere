// w4-Q: core/projection/message-input.ts の移植の検証。
// tests/core/projection/{e2e-axis1-flow,emotion-handlers,learning-handlers,e2e-axis3-submit-guard}.test.ts の移植と
// golden (Node の submitMessage の応答と記録した utterance が一致)。
#include "check.hpp"
#include "w4q_support.hpp"

#include "base/strings.hpp"
#include "core/projection/message_input.hpp"

using namespace di;
using core::projection::submit_message;
using core::projection::SubmitMessageInput;

namespace {

struct Fixture {
    w4q::Env env;
    std::unique_ptr<core::Core> core = env.core();
    std::string sid;
    Fixture() {
        core::repositories::SessionInput s;
        s.workspaceId = "w1";
        s.title = "s";
        s.startedAt = 1;
        sid = core->repos.session.create(s);
    }
    ~Fixture() { core->close(); }
    core::projection::SubmitMessageResult submit(const std::string& raw, const std::optional<std::string>& person = std::nullopt,
                                                 const std::string& ws = "w1") {
        SubmitMessageInput in;
        in.core = core.get();
        in.workspaceId = ws;
        in.sessionId = sid;
        in.personId = person;
        in.rawContent = raw;
        return submit_message(in);
    }
};

} // namespace

TEST_CASE("e2e-axis1-flow: define → intends → refine") {
    Fixture f;
    f.submit("/define parry");
    f.submit("/intends confidence");
    f.submit("/refine timing window");
    const auto mechs = f.core->repos.mechanic.list("w1");
    CHECK_EQ(mechs.size(), std::size_t{1});
    CHECK_EQ(mechs[0].str("name"), std::string("parry"));
    CHECK_EQ(mechs[0].str("intends"), std::string("confidence"));
}

TEST_CASE("emotion-handlers: /scene → /done で session が終わる") {
    Fixture f;
    auto r = f.submit("/scene cave");
    CHECK(r.commandResult && r.commandResult->ok);
    r = f.submit("/done");
    CHECK(r.commandResult && r.commandResult->ok);
    const auto s = f.core->raw().get("SELECT ended_at FROM sessions WHERE id = ?", {f.sid});
    CHECK(s && !s->is_null("ended_at"));
}

TEST_CASE("learning-handlers: /define は mechanic を 1 件作る (utterance は作らない)") {
    Fixture f;
    const auto r = f.submit("/define dash");
    CHECK(r.commandResult && r.commandResult->ok);
    CHECK(r.utteranceId.empty());
    CHECK_EQ(f.core->repos.mechanic.list("w1").size(), std::size_t{1});
    CHECK_EQ(f.core->raw().get("SELECT COUNT(*) AS n FROM utterances")->i64("n"), std::int64_t{0});
}

TEST_CASE("e2e-axis3-submit-guard: addresses の無い /submit は拒否") {
    Fixture f;
    f.submit("/propose title");
    const auto r = f.submit("/submit");
    CHECK(r.commandResult && !r.commandResult->ok);
    CHECK(str::contains(str::to_lower(r.commandResult->error.value_or("")), "addresses"));
}

TEST_CASE("平文は utterance (speaker / respondsTo 推定)。commandResult は無い") {
    Fixture f;
    const auto r = f.submit("> @uid:u-prev\n返信です", std::string("p1"));
    CHECK(!r.utteranceId.empty());
    CHECK(!r.commandResult);
    const auto row = f.core->raw().get("SELECT speaker_id, responds_to, workspace_id FROM utterances WHERE id = ?", {r.utteranceId});
    CHECK_EQ(row->str("speaker_id"), std::string("p1"));
    CHECK_EQ(row->str("responds_to"), std::string("u-prev"));
    const auto anon = f.submit("ふつう");
    CHECK(f.core->raw().get("SELECT speaker_id FROM utterances WHERE id = ?", {anon.utteranceId})->is_null("speaker_id"));
    // JSON の形 ({utteranceId} / {utteranceId, commandResult:{ok, error}})
    CHECK_EQ(json_stringify(core::projection::to_json(f.submit("/nope"))),
             std::string(R"({"utteranceId":"","commandResult":{"ok":false,"error":"unknown command: nope"}})"));
}

TEST_CASE("core 無しは invalid_argument") {
    SubmitMessageInput in;
    in.rawContent = "x";
    CHECK_THROWS(submit_message(in));
}

TEST_CASE("巨大な本文・壊れた UTF-8 も 1 件の utterance として記録する (落ちない)") {
    Fixture f;
    const std::string big(1u << 20, 'a');
    CHECK(!f.submit(big).utteranceId.empty());
    CHECK(!f.submit(std::string("\xff\xfe broken")).utteranceId.empty());
    // コマンド名だけ巨大でも unknown command
    const auto r = f.submit("/" + std::string(100'000, 'z'));
    CHECK(r.commandResult && !r.commandResult->ok);
}

TEST_CASE("golden: Node の submitMessage と同じ応答・同じ utterance") {
    const auto fx = w4q::load_fixture("message_input.json");
    w4q::Env env;
    auto core = env.core();
    core->raw().exec("INSERT INTO sessions (id, workspace_id, title, started_at, created_at, updated_at) VALUES ('sx', 'ws', 's', 1, 1, 1)");
    for (const auto& c : fx["results"]) {
        SubmitMessageInput in;
        in.core = core.get();
        in.workspaceId = "ws";
        in.sessionId = "sx";
        in.personId = "p1";
        in.rawContent = c["rawContent"].get<std::string>();
        const auto r = submit_message(in);
        CHECK_EQ(!r.utteranceId.empty(), c["hasUtterance"].get<bool>());
        const auto actual = r.commandResult ? json_stringify(core::projection::to_json(*r.commandResult)) : std::string("null");
        CHECK_EQ(w4q::mask_uuids(actual), w4q::mask_uuids(json_stringify(c["commandResult"])));
    }
    Json rows = Json::array();
    for (const auto& r : core->raw().all("SELECT speaker_id, raw_content, responds_to FROM utterances WHERE session_id = 'sx' ORDER BY rowid"))
        rows.push_back(r.to_json());
    CHECK_EQ(json_stringify(rows), json_stringify(fx["utterances"]));
    core->close();
}

DI_TEST_MAIN()
