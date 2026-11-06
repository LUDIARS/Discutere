// adapter のテスト (1/3)。準備は adapter_support.hpp。
#include "adapter_support.hpp"

using namespace adapter_support;

TEST_CASE("discatier-adapter.test.ts: gaps / propose / active hypotheses / post utterance / defaultSessionId") {
    w3n::TempDir dir;
    w3n::TestConfig cfg(dir, false);
    auto core = core::create_core(dir.file("db.kuzu"), dir.file("events.jsonl"));
    const auto gap_id = make_gap(*core, "オンボーディング離脱");
    const auto session_id = make_session(*core, "design discussion");
    DiscatierAdapterOptions opt;
    opt.defaultSessionId = session_id;
    auto ctx = create_discatier_context_provider(*core, opt);

    auto gaps = ctx->list_recent_gaps(ws, 10);
    CHECK_EQ(gaps.size(), std::size_t{1});
    CHECK_EQ(gaps[0].id, gap_id);
    CHECK_EQ(gaps[0].title, std::string("オンボーディング離脱"));
    CHECK(gaps[0].hypothesisIds.empty());

    const auto hid = ctx->propose_hypothesis({ws, "選択肢を 2 つに絞る", gap_id, "advocate", std::nullopt});
    CHECK(!hid.empty());
    const auto hyps = core->raw().all("SELECT id, statement, design_gap_id FROM hypotheses WHERE workspace_id = ?", {ws});
    CHECK_EQ(hyps.size(), std::size_t{1});
    CHECK_EQ(hyps[0].str("statement"), std::string("選択肢を 2 つに絞る"));
    CHECK_EQ(hyps[0].str("design_gap_id"), gap_id);

    const auto active = ctx->list_active_hypotheses(ws, 10);
    CHECK_EQ(active.size(), std::size_t{1});
    CHECK_EQ(active[0].designGapId.value_or(""), gap_id);
    CHECK_EQ(ctx->list_recent_gaps(ws, 10)[0].hypothesisIds, std::vector<std::string>{hid});

    // closed gap の仮説は active から外す。
    core->raw().run("UPDATE design_gaps SET status = 'closed' WHERE id = ?", {gap_id});
    CHECK(ctx->list_active_hypotheses(ws, 10).empty());

    const auto utt = ctx->post_utterance({ws, session_id, "そもそも 2 択でいいのか", "sceptic", std::nullopt});
    const auto utts = ctx->list_recent_utterances(ws, session_id, 10);
    CHECK_EQ(utts.size(), std::size_t{1});
    CHECK_EQ(utts[0].rawContent, std::string("そもそも 2 択でいいのか"));
    CHECK_EQ(core->raw().get("SELECT speaker_id FROM utterances WHERE id = ?", {utt})->str("speaker_id"), std::string("persona:sceptic"));
    CHECK(!ctx->post_utterance({ws, "", "default session fallback", "advocate", std::nullopt}).empty());
    // defaultSessionId も sessionId も無ければ投げる。
    auto bare = create_discatier_context_provider(*core);
    CHECK_THROWS(bare->post_utterance({ws, "", "x", "advocate", std::nullopt}));
    core->close();
}

TEST_CASE("find-discussion-session.test.ts: gap / hypothesis から解決・ended / closed は対象外") {
    w3n::TempDir dir;
    w3n::TestConfig cfg(dir, false);
    auto core = core::create_core(dir.file("db.kuzu"), dir.file("events.jsonl"));
    auto adapter = create_discatier_context_provider(*core);
    const auto gap_id = make_gap(*core, "面白さの本質", std::nullopt);
    const auto session_id = make_session(*core, "discussion-of-gap:" + gap_id);
    core::repositories::HypothesisInput h;
    h.workspaceId = ws;
    h.designGapId = gap_id;
    h.statement = "自動攻撃が手軽さを生む";
    const auto hid = core->repos.hypothesis.create(h);

    CHECK_EQ(adapter->find_discussion_session({ws, std::nullopt, gap_id}).value_or(""), session_id);
    CHECK_EQ(adapter->find_discussion_session({ws, hid, std::nullopt}).value_or(""), session_id);
    CHECK(!adapter->find_discussion_session({ws, std::string("nope"), std::nullopt}));
    CHECK(!adapter->find_discussion_session({ws, std::nullopt, std::string("nope")}));
    core->raw().run("UPDATE sessions SET ended_at = ? WHERE id = ?", {time::now_ms(), session_id});
    CHECK(!adapter->find_discussion_session({ws, std::nullopt, gap_id}));

    const auto gap2 = make_gap(*core, "進行中の議題", std::nullopt);
    const auto active = make_session(*core, "discussion-of-gap:" + gap2, "discord:g1/c1");
    CHECK_EQ(adapter->find_active_discussion_session(ws).value_or(""), active);
    core->raw().run("UPDATE design_gaps SET status = 'closed' WHERE id = ?", {gap2});
    CHECK(!adapter->find_active_discussion_session(ws));
    // converged gap の議論にも着地させない。
    const auto gap3 = make_gap(*core, "収束済み", std::string("converged"));
    make_session(*core, "discussion-of-gap:" + gap3);
    CHECK(!adapter->find_discussion_session({ws, std::nullopt, gap3}));
    core->close();
}

TEST_CASE("auto-export.test.ts: autoExportRootDir で hypothesis / utterance の md を書く・未指定なら書かない・callback の例外は止めない") {
    w3n::TempDir dir;
    w3n::TestConfig cfg(dir, false);
    auto core = core::create_core(dir.file("db.kuzu"), dir.file("events.jsonl"));
    const auto session_id = make_session(*core, "auto-export session");
    const std::string root = dir.file("discussions");
    std::vector<std::string> errors;
    std::vector<PostedHypothesis> posted;
    DiscatierAdapterOptions opt;
    opt.defaultSessionId = session_id;
    opt.autoExportRootDir = root;
    opt.onAutoExportError = [&](const std::string& e, const std::string& type, const std::string&) { errors.push_back(type + ":" + e); };
    opt.onPostedHypothesis = [&](const PostedHypothesis& p) { posted.push_back(p); };
    opt.onPostedUtterance = [](const PostedUtterance&) { throw std::runtime_error("discord down"); };
    auto adapter = create_discatier_context_provider(*core, opt);

    const auto hid = adapter->propose_hypothesis({ws, "auto-exported hypothesis", std::nullopt, "advocate", std::nullopt});
    const auto hyp_md = fs::read_file(root + "/" + ws + "/hypothesis/" + hid + ".md");
    CHECK(hyp_md && str::contains(*hyp_md, "auto-exported hypothesis"));
    CHECK_EQ(posted.size(), std::size_t{1});
    CHECK(!posted[0].designGapId);
    CHECK_EQ(posted[0].hypothesisId, hid);

    const auto uid = adapter->post_utterance({ws, session_id, "auto-exported utterance", "sceptic", std::nullopt});
    const auto utt_md = fs::read_file(root + "/" + ws + "/utterance/" + uid + ".md");
    CHECK(utt_md && str::contains(*utt_md, "auto-exported utterance"));
    CHECK_EQ(errors.size(), std::size_t{1});
    CHECK(str::contains(errors[0], "utt:discord down"));

    std::size_t before = 0;
    for (const auto& e : std::filesystem::recursive_directory_iterator(root)) before += e.is_regular_file();
    DiscatierAdapterOptions opt2;
    opt2.defaultSessionId = session_id;
    create_discatier_context_provider(*core, opt2)->propose_hypothesis({ws, "no auto-export", std::nullopt, "refiner", std::nullopt});
    std::size_t after = 0;
    for (const auto& e : std::filesystem::recursive_directory_iterator(root)) after += e.is_regular_file();
    CHECK_EQ(after, before);
    core->close();
}

DI_TEST_MAIN()
