// 議論終了時に Discord へ送るまとめ・チャット風 HTML の描画 (DB を使わない部分) の確認。
// DB を使う組み立ては transcript_tests.cpp (静的解析の 1 翻訳単位を軽く保つため分けた)。
#include "check.hpp"

#include "base/json.hpp"
#include "base/strings.hpp"
#include "flow/transcript/digest.hpp"
#include "flow/transcript/html.hpp"
#include "flow/transcript/paper_md_html.hpp"

using namespace di;
using namespace di::flow::transcript;

namespace {

bool has(const std::string& s, std::string_view needle) { return s.find(needle) != std::string::npos; }

std::size_t count_of(const std::string& s, std::string_view needle) {
    std::size_t n = 0;
    for (auto p = s.find(needle); p != std::string::npos; p = s.find(needle, p + needle.size())) ++n;
    return n;
}

Json utterance(const std::string& id, std::int64_t round, const std::string& name, const std::string& stance, const std::string& role,
               const std::string& text, std::int64_t votes = 0, bool winner = false) {
    return Json{{"id", id},       {"round", round},          {"turn", 1},          {"personaName", name}, {"displayName", name + " (" + role + ")"},
                {"role", role},   {"stance", stance},        {"possessionName", nullptr}, {"text", text}, {"isError", false},
                {"votes", votes}, {"isWinner", winner},      {"roundAufhebung", winner ? Json::array({"a", "b"}) : Json::array()},
                {"createdAt", 1}};
}

} // namespace

TEST_CASE("paper md: 見出し・箇条書き・太字だけを変換し、HTML はエスケープする") {
    CHECK_EQ(render_paper_md_html("# 議題\n本文 **強調** <script>\n\n## 小見出し\n- 一\n* 二\n\n段落"),
             std::string("<h3>議題</h3><p>本文 <strong>強調</strong> &lt;script&gt;</p><h4>小見出し</h4><ul><li>一</li><li>二</li></ul><p>段落</p>"));
    CHECK_EQ(render_paper_md_html("#見出しでない\n**閉じない"), std::string("<p>#見出しでない</p><p>**閉じない</p>"));
    CHECK_EQ(render_paper_md_html("***b**"), std::string("<p><strong>*b</strong></p>"));
    CHECK_EQ(render_paper_md_html(""), std::string());
}

TEST_CASE("html: 立場のクラス・印・結論・エスケープ") {
    TranscriptHtmlInput in;
    in.theme = "テーマ <x> & \"q\"";
    in.flowLabel = "企画/議論";
    in.generatedAtMs = 1700000000000;
    in.snapshot.utterances = Json::array({utterance("u1", 1, "アリス", "pro", "persona", "<img src=x onerror=alert(1)>"),
                                          utterance("u2", 2, "ボブ", "con", "persona", "反対", 3, true),
                                          utterance("u3", 2, "人", "neutral", "user", "質問"),
                                          utterance("u4", 2, "中", "opinion", "persona", "中立意見")});
    in.snapshot.conclusion = "結論 <b>";
    in.snapshot.concluded = true;
    in.snapshot.paperMd = "# 議題\n- 観点";
    const auto html = render_transcript_html(in);
    CHECK(str::starts_with(html, "<!doctype html>"));
    CHECK(!has(html, "{{"));
    CHECK(!has(html, "<script"));
    CHECK(!has(html, "<img"));
    CHECK(has(html, "&lt;img src=x onerror=alert(1)&gt;"));
    CHECK(has(html, "<title>テーマ &lt;x&gt; &amp; &quot;q&quot; — Discutere 議論ログ</title>"));
    CHECK(has(html, "class=\"u pro\""));
    CHECK(has(html, "class=\"u con\""));
    CHECK(has(html, "class=\"u user\""));
    CHECK(has(html, "class=\"u opinion\""));
    CHECK(has(html, "<span class=\"badge\">賛成派</span>"));
    CHECK(has(html, "<span class=\"badge\">反対派</span>"));
    CHECK(has(html, "<span class=\"badge\">人間</span>"));
    CHECK(has(html, "<span class=\"mark vote\">投票 3</span><span class=\"mark win\">採択</span><span class=\"mark auf\">止揚 2</span>"));
    CHECK(has(html, "<div id=\"conclusionBody\">結論 &lt;b&gt;</div>"));
    CHECK(has(html, "<h3>議題</h3><ul><li>観点</li></ul>"));
    CHECK(has(html, "発言 4 件 / ラウンド 2 / 結論あり / 出力 2023-11-14 22:13 (UTC)"));
    CHECK(has(html, "prefers-color-scheme: dark"));
    CHECK(has(html, "<div id=\"live\" class=\"\">"));
}

TEST_CASE("html: 空データ・上限で省く") {
    TranscriptHtmlInput in;
    const auto empty = render_transcript_html(in);
    CHECK(has(empty, "発言はありません。"));
    CHECK(has(empty, "(議題なし)"));
    CHECK(has(empty, "class=\"no-paper\""));
    CHECK(!has(empty, "id=\"conclusion\""));
    CHECK(!has(empty, "id=\"paperPanel\""));
    for (int i = 0; i < 10; ++i) in.snapshot.utterances.push_back(utterance("u" + std::to_string(i), 1, "a", "pro", "persona", std::string(100, 'x')));
    const auto capped = render_transcript_html(in, 700);
    CHECK(count_of(capped, "class=\"u pro\"") < 10);
    CHECK(has(capped, "件は容量の上限を超えるため省きました。"));
}

TEST_CASE("digest: 規模・立場別・採択・結論を 1900 字以内で") {
    DigestInput in;
    in.theme = "周回の報酬";
    in.flowLabel = "企画/議論";
    in.snapshot.utterances = Json::array({utterance("u1", 1, "アリス", "pro", "persona", "賛成\n理由"),
                                          utterance("u2", 2, "ボブ", "con", "persona", "反対", 2, true),
                                          utterance("u3", 2, "人", "neutral", "user", "質問")});
    in.snapshot.conclusion = "結論です";
    const auto d = render_transcript_digest(in);
    CHECK(has(d, "📄 **議論のまとめ** — 周回の報酬"));
    CHECK(has(d, "企画/議論 / ラウンド 2 / 発言 3 件 (賛成派 1 / 反対派 1 / 人間 1)"));
    CHECK(has(d, "**採択された意見**\n- ボブ (persona): 反対"));
    CHECK(has(d, "**結論**\n結論です"));
    CHECK(has(d, "添付の HTML"));

    // 長い結論・多数の採択でも上限に収める
    in.snapshot.conclusion = std::string(5000, 'c');
    for (int i = 0; i < 20; ++i) in.snapshot.utterances.push_back(utterance("w" + std::to_string(i), 3, "名", "pro", "persona", std::string(1000, 'y'), 1, true));
    const auto big = render_transcript_digest(in);
    CHECK(str::js_length(big) <= kDigestMaxLength);
    CHECK(has(big, "- ほか 16 件"));
    CHECK(has(big, "添付の HTML"));

    DigestInput none;
    const auto e = render_transcript_digest(none);
    CHECK(has(e, "(議題なし)"));
    CHECK(has(e, "発言 0 件"));
    CHECK(has(e, "(明確な結論には至りませんでした)"));
}

DI_TEST_MAIN()
