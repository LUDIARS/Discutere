// w3-L: fundamentum-export (snapshot / export / list) のテスト (tests/fundamentum-export/run.ts の観点 + golden)。
// content-addressed なので snapshot に可変値 (export 時刻等) を入れない: 同じ detail からは同じ content id。
#include "check.hpp"
#include "flow/w3l_support.hpp"

#include "fundamentum_export/export.hpp"
#include "fundamentum_export/list.hpp"
#include "fundamentum_export/snapshot.hpp"
#include "fundamentum_export/store.hpp"
#include "lib/fundamentum/fundamentum.hpp"

using namespace di;
using namespace di::fundamentum_export;
using w3l::Json;

namespace {

visualize::ConclusionDetail make_detail() {
    return visualize::conclusion_detail_from_json(Json::parse(R"({
      "gapId": "flow:session-1", "kind": "flow", "sessionId": "session-1", "title": "テスト議論", "conclusion": "結論本文",
      "utteranceCount": 3, "aufhebungCount": 1, "updatedAt": 1700000000000,
      "paper": {"theme": "テーマ", "tags": ["開発"], "supplement": "", "mechanics": []},
      "aufhebungen": ["止揚A"],
      "topOpinions": [{"speaker": "論者#1", "content": "意見1", "score": 5, "source": null, "sourceUrl": null}],
      "transcript": [{"speaker": "論者#1", "content": "発話1", "source": null, "sourceUrl": null}]
    })"));
}

} // namespace

TEST_CASE("golden: build_discussion_snapshot と content id が Node と同じ") {
    for (const auto& c : w3l::golden()["snapshots"]) {
        const auto detail = visualize::conclusion_detail_from_json(c["detail"]);
        const Json snap = build_discussion_snapshot(detail);
        W3L_SAME(snap, c["snapshot"]);
        CHECK_EQ(lib::fundamentum::content_id_of(snap), c["contentId"].get<std::string>());
    }
}

TEST_CASE("snapshot: 同じ detail からは同じ snapshot (可変値を含まない)") {
    const Json a = build_discussion_snapshot(make_detail());
    const Json b = build_discussion_snapshot(make_detail());
    CHECK_EQ(json_stringify(a), json_stringify(b));
    CHECK_EQ(a["discussionId"].get<std::string>(), std::string("flow:session-1"));
    CHECK(!a.contains("exportedAt"));
    CHECK(!a.contains("sessionId"));
}

TEST_CASE("export → list → get → history を in-memory Foundation で往復 (run.ts)") {
    set_fundamentum_store_for_tests(std::shared_ptr<lib::fundamentum::Foundation>(lib::fundamentum::Foundation::in_memory()));
    const auto detail = make_detail();
    const auto r1 = export_discussion_to_fundamentum(detail);
    CHECK_EQ(r1.discussionId, detail.gapId);
    CHECK_EQ(r1.catalogVersion, 1);
    const auto listed = list_exported_discussions();
    CHECK_EQ(listed.size(), 1u);
    CHECK_EQ(listed[0].discussionId, detail.gapId);
    const auto snap = get_exported_discussion_snapshot(detail.gapId);
    CHECK(snap.has_value());
    CHECK_EQ((*snap)["title"].get<std::string>(), std::string("テスト議論"));
    CHECK(!get_exported_discussion_snapshot("nope").has_value());

    const auto r1b = export_discussion_to_fundamentum(make_detail());
    CHECK_EQ(r1b.contentId, r1.contentId);
    CHECK_EQ(r1b.catalogVersion, 1);

    auto changed = make_detail();
    changed.conclusion = "更新後の結論";
    const auto r2 = export_discussion_to_fundamentum(changed);
    CHECK_EQ(r2.catalogVersion, 2);
    CHECK(r2.contentId != r1.contentId);

    // 他の議論の export でも catalog の版は進むが、この議論の履歴には変化点だけが残る。
    auto other = make_detail();
    other.gapId = "flow:a-first";
    const auto r3 = export_discussion_to_fundamentum(other);
    CHECK_EQ(r3.catalogVersion, 3);
    const auto history = get_discussion_export_history(detail.gapId);
    CHECK_EQ(history.size(), 2u);
    CHECK_EQ(history[0].contentId, r1.contentId);
    CHECK_EQ(history[1].contentId, r2.contentId);
    CHECK_EQ(history[1].version, 2);
    const auto all = list_exported_discussions();
    CHECK_EQ(all.size(), 2u);
    CHECK_EQ(all[0].discussionId, std::string("flow:a-first"));  // discussionId の順
    CHECK(get_discussion_export_history("nope").empty());
    W3L_SAME(to_json(r2), (Json{{"discussionId", "flow:session-1"}, {"contentId", r2.contentId}, {"catalogVersion", 2}}));
    set_fundamentum_store_for_tests(nullptr);
}

int main() { return di::test::run_all(); }
