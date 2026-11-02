// w2-H: src/flow/notion-link.ts の移植の確認 (tests/flow/notion-link.test.ts + golden)。
// 取得は差し替え (偽の crawler)。サイドカー native/sidecar/canalis-crawl.ts は node があれば実際に起動して入力検査を確かめ、
// 取得手順 (token あり=crawlPage で深さ N / 無し=公開ページ) は native/tests/golden/w2-H-canalis-crawl.check.ts で確かめる。
#include "w2h_support.hpp"

#include <chrono>
#include <iostream>

#include "base/process.hpp"
#include "base/sidecar.hpp"
#include "base/strings.hpp"
#include "flow/notion_link.hpp"

using namespace di;
using namespace di::flow;

namespace {

std::optional<std::string> repo_root_with_node() {
    auto root = env::get_nonempty("DI_REPO_ROOT");
    if (!root || !proc::find_executable("node", env::snapshot())) return std::nullopt;
    if (!fs::is_dir(fs::join(*root, "node_modules/tsx")) || !fs::exists(fs::join(*root, "node_modules/@ludiars/canalis"))) return std::nullopt;
    env::set_for_test("DISCUTERE_REPO_ROOT", *root);
    return root;
}

} // namespace

TEST_CASE("depth directive and URL detection match Node / Canalis (golden)") {
    for (const auto& c : w2h::fx()["notionDepth"]) {
        const auto d = parse_notion_depth(c["text"].get<std::string>());
        CHECK_EQ(w2h::s(d ? Json(*d) : Json(nullptr)), w2h::s(c["depth"]));
    }
    for (const auto& c : w2h::fx()["notionUrls"]) CHECK_EQ(w2h::s(Json(find_notion_urls(c["text"].get<std::string>()))), w2h::s(c["urls"]));
    for (const auto& c : w2h::fx()["isNotion"]) CHECK_EQ(is_notion_url(c["u"].get<std::string>()), c["ok"].get<bool>());
}

TEST_CASE("expand: md, headings by depth, warnings and failure isolation match Node (golden)") {
    const auto& g = w2h::fx()["notionExpand"];
    std::vector<std::string> warns;
    ExpandNotionOpts o;
    o.warn = [&](const std::string& m) { warns.push_back(m); };
    o.crawler = [](const std::string& url, int depth, int max_pages) {
        if (url.find("Other") != std::string::npos) throw std::runtime_error("timeout");
        NotionCrawlResult r;
        r.pages = {{url, "公開ページ", 0, "公開本文\n"}, {"", "", 1, "子本文"}, {"https://acme.notion.site/Deep", "深い", 7, ""}};
        // サイドカーは取得時の warn を順に返す (移植前の文言)
        r.warnings.push_back("Notion 公開ページ取得失敗 (https://acme.notion.site/Gone): 404");
        if (depth == 1) r.warnings.push_back("Notion ページ数上限 " + std::to_string(max_pages) + " で打ち切り (" + url + ")");
        return r;
    };
    CHECK_EQ(expand_notion_links(g["text"].get<std::string>(), o), g["md"].get<std::string>());
    CHECK_EQ(w2h::s(Json(warns)), w2h::s(g["warns"]));
}

TEST_CASE("expand: no links is empty, defaults and overrides of depth / pages (notion-link.test.ts)") {
    std::vector<std::pair<int, int>> seen;
    ExpandNotionOpts o;
    o.crawler = [&](const std::string& url, int depth, int max_pages) {
        seen.emplace_back(depth, max_pages);
        return NotionCrawlResult{{{url, "T", 0, "本文"}}, {}};
    };
    CHECK_EQ(expand_notion_links("https://example.com/x だけ", o), std::string());
    const std::string url = "https://www.notion.so/ws/Spec-0123456789abcdef0123456789abcdef";
    CHECK(str::starts_with(expand_notion_links(url + "。", o), "# Notion 資料\n\n## T\n出典: " + url));
    expand_notion_links(url + " 深さ:0", o);
    expand_notion_links(url + " 深さ:99", o);
    o.depth = 9;
    o.maxPages = 100000;
    expand_notion_links(url, o);
    CHECK_EQ(seen, (std::vector<std::pair<int, int>>{{2, 50}, {0, 50}, {5, 50}, {5, MAX_NOTION_PAGES}}));
    // 取得失敗は warn して空
    std::vector<std::string> warns;
    ExpandNotionOpts f;
    f.crawler = [](const std::string&, int, int) -> NotionCrawlResult { throw std::runtime_error("timeout"); };
    f.warn = [&](const std::string& m) { warns.push_back(m); };
    CHECK_EQ(expand_notion_links(url, f), std::string());
    CHECK(warns.size() == 1 && warns[0].find("timeout") != std::string::npos);
}

TEST_CASE("boundary: huge text is scanned linearly, links are capped") {
    const auto t0 = std::chrono::steady_clock::now();
    std::string text;
    for (int i = 0; i < 200'000; ++i) text += "https:// depth:: 深さ ";
    CHECK(find_notion_urls(text).empty());
    CHECK(!parse_notion_depth(text));
    CHECK(find_notion_urls("https://" + std::string(2'000'000, 'a')).empty());
    CHECK_EQ(*parse_notion_depth("depth:" + std::string(100'000, '9')), MAX_NOTION_DEPTH);
    std::string many;
    for (std::size_t i = 0; i < MAX_NOTION_LINKS + 5; ++i) many += "https://acme.notion.site/p" + std::to_string(i) + " ";
    CHECK_EQ(find_notion_urls(many).size(), MAX_NOTION_LINKS + 5);
    int calls = 0;
    std::vector<std::string> warns;
    ExpandNotionOpts o;
    o.crawler = [&](const std::string& url, int, int) {
        ++calls;
        return NotionCrawlResult{{{url, "T", 0, ""}}, {}};
    };
    o.warn = [&](const std::string& m) { warns.push_back(m); };
    expand_notion_links(many, o);
    CHECK_EQ(calls, static_cast<int>(MAX_NOTION_LINKS));
    CHECK_EQ(warns.size(), 1u);
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(10));
    CHECK_EQ(notion_sidecar_timeout(1).count(), 90'000);
    CHECK_EQ(notion_sidecar_timeout(200).count(), 15 * 60'000);
}

TEST_CASE("real sidecar: input bounds are checked in Node (no network)") {
    if (!repo_root_with_node()) {
        std::cout << "skip: node / tsx / canalis が無い\n";
        return;
    }
    sidecar::Options o;
    o.timeout = std::chrono::seconds(60);
    const std::string url = "https://www.notion.so/ws/Spec-0123456789abcdef0123456789abcdef";
    for (const Json& bad : {Json{{"url", "https://example.com/x"}, {"depth", 1}, {"maxPages", 1}},
                            Json{{"url", url}, {"depth", MAX_NOTION_DEPTH + 1}, {"maxPages", 1}},
                            Json{{"url", url}, {"depth", 1}, {"maxPages", MAX_NOTION_PAGES + 1}},
                            Json{{"url", url}, {"depth", 1}, {"maxPages", 0}}, Json::array()}) {
        const auto r = sidecar::run("canalis-crawl.ts", bad, o);
        CHECK(!r.ok);
        CHECK_EQ(r.error, std::string("invalid sidecar input"));
    }
    // 上限を超える入力は読み切らずに失敗する
    const auto big = sidecar::run("canalis-crawl.ts", Json{{"url", url}, {"pad", std::string(64 * 1024, 'x')}}, o);
    CHECK(!big.ok);
    CHECK(big.error.find("too large") != std::string::npos);
}

TEST_CASE("sidecar crawl steps with real Canalis crawlPage and fake Notion API (w2-H-canalis-crawl.check.ts)") {
    auto root = repo_root_with_node();
    if (!root) {
        std::cout << "skip: node / tsx / canalis が無い\n";
        return;
    }
    proc::Options p;
    p.argv = {"node", "--import", "tsx", fs::join(*root, "native/tests/golden/w2-H-canalis-crawl.check.ts")};
    p.cwd = *root;
    p.timeout = std::chrono::seconds(90);
    const auto r = proc::run(p);
    CHECK_EQ(r.exit_code, 0);
    const Json out = parse_json_or_throw(str::trim(r.out));
    CHECK_EQ(w2h::s(out["depth2"]), std::string(R"(["0:T-01234567-89ab-cdef-0123-456789abcdef","1:T-c1","2:T-c2"])"));
    CHECK_EQ(out["depth0"].size(), 1u);
    CHECK_EQ(out["depth3"].size(), 4u);
    CHECK(out["truncated"].dump().find("ページ数上限 2 で打ち切り") != std::string::npos);
    CHECK(out["apiFallback"]["warnings"].dump().find("公開ページ取得へ切替") != std::string::npos);
    CHECK_EQ(out["noToken"]["pages"][0]["title"].get<std::string>(), std::string("公開ページ"));
    CHECK_EQ(w2h::s(out["noToken"]["warnings"]),
             std::string(R"x(["Notion 公開ページ取得失敗 (https://acme.notion.site/Gone): 404","Notion ページ数上限 7 で打ち切り (https://www.notion.so/ws/Spec-0123456789abcdef0123456789abcdef)"])x"));
    CHECK_EQ(w2h::s(out["inputs"]), std::string("[true,true,false,false,false,false,false,false,false,false]"));
}

DI_TEST_MAIN()
