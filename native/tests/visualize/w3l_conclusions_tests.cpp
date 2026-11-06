// w3l_conclusions のテスト (1/3)。準備は w3l_conclusions_support.hpp。
#include "w3l_conclusions_support.hpp"

using namespace w3l_conclusions_support;

TEST_CASE("golden: safe_slug / render_conclusion_markdown (Node と 1 バイトも違わない)") {
    for (const auto& c : w3l::golden()["slugs"])
        CHECK_EQ(safe_slug(c["title"].get<std::string>(), c["fallback"].get<std::string>()), c["slug"].get<std::string>());
    for (const auto& c : w3l::golden()["markdown"]) {
        const auto detail = conclusion_detail_from_json(c["detail"]);
        W3L_SAME(to_json(detail), c["detail"]);  // JSON の往復でも値が変わらない
        CHECK_EQ(render_conclusion_markdown(detail), c["md"].get<std::string>());
    }
}

TEST_CASE("conclusion-markdown.test.ts の観点 (front matter / 節 / 出所 / paper なし / 未生成)") {
    const auto detail = conclusion_detail_from_json(w3l::golden()["markdown"][0]["detail"]);
    const auto parsed = lib::front_matter::parse(render_conclusion_markdown(detail));
    CHECK(parsed.has_value());
    CHECK_EQ(parsed->data["type"].get<std::string>(), std::string("discussion-conclusion"));
    CHECK_EQ(parsed->data["kind"].get<std::string>(), std::string("flow"));
    CHECK_EQ(parsed->data["id"].get<std::string>(), std::string("flow:sess-1"));
    CHECK(parsed->data["concluded"].get<bool>());
    CHECK(str::contains(parsed->content, "## 議論ログ (3 発話)"));
    CHECK(str::contains(parsed->content, "([youtube-livechat](https://youtu.be/x))"));
    CHECK(str::contains(parsed->content, "**ビルド選択**: 周回ごとに構成を変える → 期待感情: 発見"));
    auto no_paper = detail;
    no_paper.paper.reset();
    CHECK(!str::contains(render_conclusion_markdown(no_paper), "## ディスカッションペーパー"));
    auto no_conc = detail;
    no_conc.conclusion.reset();
    const auto p2 = lib::front_matter::parse(render_conclusion_markdown(no_conc));
    CHECK(!p2->data["concluded"].get<bool>());
    CHECK(str::contains(p2->content, "(まとめ未生成)"));
    // Date の範囲: ちょうど ±8.64e15 は書ける、+1 は RangeError と同じく例外。
    auto edge = detail;
    edge.updatedAt = 8'640'000'000'000'000LL;
    (void)render_conclusion_markdown(edge);
    edge.updatedAt += 1;
    CHECK_THROWS(render_conclusion_markdown(edge));
    // 巨大なタイトル: スラグは 60 UTF-16 単位まで
    CHECK_EQ(str::js_length(safe_slug(std::string(1u << 20, 'a'), "x")), 60u);
}

int main() { return di::test::run_all(); }
