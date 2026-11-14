// 議論終了時に Discord へ送るまとめ + チャット風 HTML のうち、DB から組み立てる部分 (snapshot / discord message) の確認。
#include "check.hpp"
#include "w2h_support.hpp"

#include "base/strings.hpp"
#include "flow/db/connection.hpp"
#include "flow/transcript/discord_message.hpp"
#include "flow/transcript/snapshot.hpp"

using namespace di;
using namespace di::flow::transcript;

namespace {

bool has(const std::string& s, std::string_view needle) { return s.find(needle) != std::string::npos; }

void seed(const std::string& sid) {
    auto db = flow::db::get_flow_db();
    const char* ins =
        "INSERT INTO flow_utterance (id, session_id, paper_id, round, turn, persona_id, persona_name, role, stance, text, is_error, created_at)"
        " VALUES (?, ?, 'p', ?, ?, 'pid', ?, ?, ?, ?, 0, ?)";
    db->run(ins, {"u1", sid, 1, 1, "アリス", "persona", "pro", "賛成です <b>", 100});
    db->run(ins, {"u2", sid, 1, 2, "ボブ", "persona", "con", "反対です", 200});
    db->run(ins, {"u3", sid, 2, 1, "人間さん", "user", "neutral", "質問", 300});
    db->run("INSERT INTO vote (session_id, round, voter_index, chosen_utterance_id, created_at) VALUES (?, 1, 0, 'u2', 1)", {sid});
    db->run("INSERT INTO vote (session_id, round, voter_index, chosen_utterance_id, created_at) VALUES (?, 1, 1, 'u2', 1)", {sid});
    db->run("INSERT INTO flow_conclusion (session_id, paper_id, summary, concluded, created_at) VALUES (?, 'p', '結論です', 1, 1)", {sid});
}

} // namespace

TEST_CASE("snapshot: 発話・投票・採択・結論を status と同じ形で返す") {
    w2h::TempFlowDb tmp;
    seed("s1");
    const auto snap = load_transcript_snapshot("s1");
    CHECK_EQ(snap.utterances.size(), std::size_t(3));
    const auto& u2 = snap.utterances[1];
    std::vector<std::string> keys;
    for (auto it = u2.begin(); it != u2.end(); ++it) keys.push_back(it.key());
    // status の utterances と同じキー順
    CHECK(keys == std::vector<std::string>({"id", "round", "turn", "personaName", "displayName", "role", "stance", "possessionName", "text",
                                            "isError", "votes", "isWinner", "roundAufhebung", "createdAt"}));
    CHECK_EQ(u2["votes"].get<int>(), 2);
    CHECK(u2["isWinner"].get<bool>());
    CHECK(!snap.utterances[0]["isWinner"].get<bool>());
    CHECK_EQ(snap.marks.size(), std::size_t(1));
    CHECK_EQ(snap.marks[0]["id"].get<std::string>(), std::string("u2"));
    CHECK_EQ(snap.conclusion.get<std::string>(), std::string("結論です"));
    CHECK(snap.concluded);
    CHECK(snap.hasConclusionRow);
    CHECK(!snap.paperMd.has_value());
    // since で絞る (marks は全期間)
    const auto later = load_transcript_snapshot("s1", 150);
    CHECK_EQ(later.utterances.size(), std::size_t(2));
    CHECK_EQ(later.marks.size(), std::size_t(1));
    // 無い議論は空
    const auto none = load_transcript_snapshot("none");
    CHECK(none.utterances.empty());
    CHECK(none.conclusion.is_null());
    CHECK(!none.hasConclusionRow);
}

TEST_CASE("discord message: DB からまとめと HTML 添付を 1 通に組む") {
    w2h::TempFlowDb tmp;
    seed("forum-s");
    const auto msg = build_transcript_message(TranscriptMessageInput{"12345", "forum-s", "テーマ", "企画/議論", 1700000000000});
    CHECK(has(msg.body["content"].get<std::string>(), "**結論**\n結論です"));
    CHECK(msg.body["allowed_mentions"]["parse"].empty());
    CHECK_EQ(msg.file.name, std::string("discussion-12345.html"));
    CHECK_EQ(msg.file.content_type, std::string("text/html; charset=utf-8"));
    CHECK(has(msg.file.data, "賛成です &lt;b&gt;"));
    CHECK(has(msg.file.data, "<span class=\"mark vote\">投票 2</span><span class=\"mark win\">採択</span>"));
    CHECK_EQ(transcript_file_name("a/../b c"), std::string("discussion-a____b_c.html"));
    CHECK_EQ(transcript_file_name(""), std::string("discussion-thread.html"));
}

DI_TEST_MAIN()
