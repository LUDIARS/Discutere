// w3-K: crawl チャンネル (tests/discord-hook/crawl-channel.test.ts) + golden、取り込み (偽の取得)、
// crawl_handler (偽の Discord REST: リアクション・「データ追加」通知・元チャンネルへの返信フォールバック)。
#include <mutex>

#include "check.hpp"
#include "core/test_util.hpp"

#include "base/strings.hpp"
#include "core/index.hpp"
#include "discord_hook/crawl_channel.hpp"
#include "discord_hook/crawl_handler.hpp"
#include "discord_hook/system_channel.hpp"
#include "w1d_support.hpp"

using namespace di;
using namespace di::discord_hook;
namespace cs = di::crawler::sources;

namespace {

Json strings(const std::vector<std::string>& v) {
    Json a = Json::array();
    for (const auto& s : v) a.push_back(s);
    return a;
}

Json parsed_json(const ParsedCrawlMessage& p) {
    Json j = Json::object();
    if (p.gameSlug) j["gameSlug"] = *p.gameSlug;
    j["urls"] = strings(p.urls);
    return j;
}

} // namespace

TEST_CASE("golden: extract_urls / parse_crawl_message / classify / slug / summary") {
    const Json f = test::load_fixture("w3k/crawl_channel.json");
    for (const auto& c : f["extractUrls"]) CHECK_EQ(json_stringify(strings(extract_urls(c["text"].get<std::string>()))), json_stringify(c["out"]));
    for (const auto& c : f["parseCrawlMessage"]) {
        const auto p = parse_crawl_message(c["text"].get<std::string>());
        w1d::check_json_eq(parsed_json(p), c["out"], c["text"].get<std::string>().c_str());
    }
    for (const auto& c : f["classify"]) {
        const auto t = classify_crawl_url(c["url"].get<std::string>());
        CHECK_EQ(json_stringify(to_json(t)), json_stringify(c["target"]));
        CHECK_EQ(slug_from_target(t), c["slug"].get<std::string>());
    }
    for (const auto& c : f["summaries"]) {
        std::vector<CrawlIngestResult> rs;
        for (const auto& r : c["results"]) {
            CrawlIngestResult x{r["url"].get<std::string>(), r["kind"].get<std::string>(), r["gameSlug"].get<std::string>(), r["fetched"].get<int>(),
                                r["imported"].get<int>(), r["skipped"].get<int>(), std::nullopt};
            if (r.contains("error")) x.error = r["error"].get<std::string>();
            CHECK_EQ(json_stringify(to_json(x)), json_stringify(r));
            rs.push_back(x);
        }
        CHECK_EQ(format_crawl_summary(rs), c["text"].get<std::string>());
    }
}

TEST_CASE("crawl-channel.test.ts: parse / classify / slug / summary") {
    CHECK((extract_urls("見て https://a.test/x これと http://b.test/y。") == std::vector<std::string>{"https://a.test/x", "http://b.test/y"}));
    CHECK(extract_urls("URL なし").empty());
    CHECK((parse_crawl_message("[sotc] https://a.test/x") == ParsedCrawlMessage{std::string("sotc"), {"https://a.test/x"}}));
    CHECK((parse_crawl_message("sotc: https://a.test/x https://a.test/y") == ParsedCrawlMessage{std::string("sotc"), {"https://a.test/x", "https://a.test/y"}}));
    CHECK((parse_crawl_message("https://a.test/x") == ParsedCrawlMessage{std::nullopt, {"https://a.test/x"}}));
    const auto st = classify_crawl_url("https://store.steampowered.com/app/1794680/Vampire_Survivors/");
    CHECK(st.kind == "steam" && st.appId == 1794680LL);
    CHECK_EQ(classify_crawl_url("https://www.youtube.com/watch?v=dQw4w9WgXcQ").videoId.value_or(""), std::string("dQw4w9WgXcQ"));
    CHECK_EQ(classify_crawl_url("https://youtu.be/abc123XYZ_-").videoId.value_or(""), std::string("abc123XYZ_-"));
    CHECK_EQ(classify_crawl_url("https://famitsu.com/news/x.html").kind, std::string("website"));
    CHECK_EQ(classify_crawl_url("https://www.youtube.com/results?search_query=x").kind, std::string("website"));
    const auto rd = classify_crawl_url("https://www.reddit.com/r/gaming/comments/abc123/some_title/");
    CHECK(rd.kind == "reddit" && rd.submissionId == std::string("abc123"));
    CHECK_EQ(slug_from_target(CrawlTarget{"steam", "x", 42, std::nullopt, std::nullopt, std::nullopt}), std::string("steam-42"));
    CHECK_EQ(slug_from_target(CrawlTarget{"youtube", "x", std::nullopt, std::string("vid"), std::nullopt, std::nullopt}), std::string("yt-vid"));
    CHECK_EQ(slug_from_target(CrawlTarget{"reddit", "x", std::nullopt, std::nullopt, std::string("abc"), std::nullopt}), std::string("reddit-abc"));
    CHECK_EQ(slug_from_target(CrawlTarget{"website", "https://www.example.com/p", std::nullopt, std::nullopt, std::nullopt, std::nullopt}),
             std::string("example-com"));
    CHECK_EQ(slug_from_target(CrawlTarget{"website", "x", std::nullopt, std::nullopt, std::nullopt, std::nullopt}), std::string("web"));
    const auto sum = format_crawl_summary({{"https://a.test", "website", "g", 1, 1, 0, std::nullopt},
                                           {"https://b.test", "steam", "steam-1", 0, 0, 0, std::string("boom")}});
    CHECK(str::contains(sum, "データ追加") && str::contains(sum, "計 1 件") && str::contains(sum, "✅") && str::contains(sum, "⚠️") &&
          str::contains(sum, "boom"));
}

TEST_CASE("解析の境界: 巨大な本文・slug の長さ・appId の桁数・壊れた UTF-8") {
    // 巨大な本文でも線形に終わる
    std::string big;
    for (int i = 0; i < 20000; ++i) big += "https://a.test/" + std::to_string(i) + " ";
    CHECK_EQ(extract_urls(big).size(), std::size_t{20000});
    CHECK(extract_urls(std::string(4u << 20, 'h')).empty());
    CHECK(extract_urls(str::repeat("http://", 200000)).empty() == false);
    // slug ヒントは 64 文字まで
    CHECK(parse_crawl_message("[" + std::string(64, 'a') + "] https://a.test/x").gameSlug.has_value());
    CHECK(!parse_crawl_message("[" + std::string(65, 'a') + "] https://a.test/x").gameSlug.has_value());
    // 18 桁を超える appId は website 扱い (移植で追加)
    CHECK_EQ(classify_crawl_url("https://store.steampowered.com/app/" + std::string(18, '9')).kind, std::string("steam"));
    CHECK_EQ(classify_crawl_url("https://store.steampowered.com/app/" + std::string(19, '9')).kind, std::string("website"));
    // 壊れた UTF-8 でも落ちない
    (void)extract_urls(std::string("https://a.test/\xff\xfe x"));
    (void)parse_crawl_message(std::string("\xc3"));
}

namespace {

std::string steam_page(int n) {
    Json reviews = Json::array();
    for (int i = 0; i < n; ++i)
        reviews.push_back(Json{{"recommendationid", "r" + std::to_string(i)},
                               {"author", Json{{"steamid", "S" + std::to_string(i)}}},
                               {"review", "レビュー" + std::to_string(i)},
                               {"timestamp_created", 1700000000 + i},
                               {"voted_up", i % 2 == 0},
                               {"votes_up", i}});
    return json_stringify(Json{{"success", 1}, {"reviews", reviews}, {"cursor", ""}});
}

struct Env {
    test::TempDir dir;
    std::vector<std::string> requested;
    std::mutex m;
    CrawlDeps deps() {
        CrawlDeps d;
        const std::string kg = dir.file("db.kuzu");
        const std::string ev = dir.file("events.jsonl");
        d.createCore = [kg, ev] { return core::create_core(kg, ev); };
        d.workspaceId = "knowledge";
        d.openIngested = [this] { return cs::open_ingested_store(dir.file(".ingested.sqlite")); };
        d.openAttribution = [this] { return cs::open_attribution_store(dir.file(".attribution.sqlite")); };
        d.openRaw = [this] { return cs::open_raw_store(dir.file(".raw.sqlite")); };
        d.fetchImpl = [this](const http::ClientRequest& r) {
            {
                std::lock_guard lock(m);
                requested.push_back(r.url);
            }
            http::ClientResponse res;
            res.transport_ok = true;
            res.status = 200;
            res.final_url = r.url;
            if (str::contains(r.url, "/appreviews/")) {
                res.body = steam_page(3);
                res.headers.set("content-type", "application/json");
            } else {
                res.body = "<html><head><title>記事</title></head><body><article><p>" + str::repeat("本文です。", 200) + "</p></article></body></html>";
                res.headers.set("content-type", "text/html; charset=utf-8");
            }
            return res;
        };
        return d;
    }
};

} // namespace

TEST_CASE("ingest_crawl_urls: steam / website (要約 2 層) を取り込み、URL ごとに独立して失敗を記録") {
    Env env;
    auto d = env.deps();
    struct Sum : cs::Summarizer {
        std::optional<std::string> summarize(const std::string&) override { return std::string("要約"); }
    };
    d.summarizer = std::make_shared<Sum>();
    const auto results = ingest_crawl_urls(
        d, CrawlIngestInput{{"https://store.steampowered.com/app/7/x/", "https://news.example.com/a", "https://youtu.be/abcdef", "https://reddit.com/comments/abc"},
                            std::nullopt});
    CHECK_EQ(results.size(), std::size_t{4});
    CHECK(results[0].kind == "steam" && results[0].gameSlug == "steam-7" && results[0].fetched == 3 && results[0].imported == 3 && !results[0].error);
    CHECK(results[1].kind == "website" && results[1].gameSlug == "news-example-com");
    CHECK_EQ(results[1].imported, 1);
    CHECK(results[2].error && *results[2].error == "YouTube API key 未設定 (DISCUTERE_YOUTUBE_API_KEY)");
    CHECK(results[3].error && *results[3].error == "Reddit 認証未設定 (DISCUTERE_REDDIT_CLIENT_ID/SECRET)");
    // 2 回目は dedup で skip
    const auto again = ingest_crawl_urls(d, CrawlIngestInput{{"https://store.steampowered.com/app/7/x/"}, std::string("hint")});
    CHECK(again[0].gameSlug == "hint" && again[0].imported == 0 && again[0].skipped == 3);
    // 要約: 本文は要約、raw は raw-store
    auto core = d.createCore();
    const auto rows = core->raw().all("SELECT raw_content FROM utterances WHERE id LIKE 'ext:website:%'");
    CHECK(rows.size() == 1 && rows[0].str("raw_content") == "要約");
    core->close();
    auto raw = d.openRaw();
    CHECK(!raw->get("missing"));
    raw->close();
    // URL 数の上限
    std::vector<std::string> many(kCrawlMaxUrlsPerMessage + 5, "https://youtu.be/abcdef");
    CHECK_EQ(ingest_crawl_urls(d, CrawlIngestInput{many, std::nullopt}).size(), kCrawlMaxUrlsPerMessage);
}

TEST_CASE("ingest_crawl_urls: website の既定取得は私設アドレスを拒否する (SSRF 対策)") {
    Env env;
    auto d = env.deps();
    d.fetchImpl = nullptr;  // 既定 (http::fetch, allow_private_network=false)
    const auto r = ingest_crawl_urls(d, CrawlIngestInput{{"http://127.0.0.1:9/x", "http://169.254.169.254/latest/meta-data/"}, std::nullopt});
    CHECK_EQ(r.size(), std::size_t{2});
    for (const auto& x : r) CHECK(x.imported == 0);
}

// ── crawl_handler ─────────────────────────────────────────────────────────

namespace {

struct FakeDiscord {
    w1d::FakeServer fake;
    std::mutex m;
    std::vector<std::string> reactions;
    std::vector<std::pair<std::string, Json>> messages;
    bool channels_forbidden = false;
    bool system_send_fails = false;
    FakeDiscord() {
        auto& r = *fake.router;
        r.get("/guilds/:id", [](http::Request&) {
            return http::Response::json(Json{{"id", "g"}, {"name", "G"}, {"owner_id", "bot"}, {"roles", Json::array()}});
        });
        r.get("/users/@me", [](http::Request&) { return http::Response::json(Json{{"id", "bot"}, {"username", "Di"}}); });
        r.get("/guilds/:id/channels", [this](http::Request&) {
            if (channels_forbidden) return http::Response::json(Json{{"message", "Missing Access"}, {"code", 50001}}, 403);
            return http::Response::json(Json::array({Json{{"id", "cat"}, {"type", 4}, {"name", "システム"}, {"guild_id", "g"}},
                                                     Json{{"id", "sys"}, {"type", 0}, {"name", "データ追加"}, {"parent_id", "cat"}, {"guild_id", "g"}}}));
        });
        r.put("/channels/:c/messages/:m/reactions/:e/@me", [this](http::Request& q) {
            std::lock_guard lock(m);
            reactions.push_back(q.param("e"));
            return http::Response::empty(204);
        });
        r.post("/channels/:id/messages", [this](http::Request& q) {
            std::lock_guard lock(m);
            if (system_send_fails && q.param("id") == "sys") return http::Response::json(Json{{"message", "Missing Permissions"}, {"code", 50013}}, 403);
            messages.emplace_back(q.param("id"), Json::parse(q.body));
            return http::Response::json(Json{{"id", "m9"}, {"channel_id", q.param("id")}});
        });
        fake.start();
    }
    DiscordApiOptions options() const {
        DiscordApiOptions o;
        o.token = "fake-token";
        o.base_url = fake.base;
        o.allow_private_network = true;
        o.max_retries = 0;
        return o;
    }
};

Message msg(const std::string& content, std::optional<std::string> guild = std::string("g")) {
    Message m;
    m.id = "msg1";
    m.channel_id = "crawl";
    m.guild_id = std::move(guild);
    m.content = content;
    return m;
}

} // namespace

TEST_CASE("handle_crawl_message: URL 無しは何もしない / 取り込み → ⏳ ✅ → 「データ追加」へ通知") {
    FakeDiscord fd;
    DiscordApi api(fd.options());
    clear_system_channel_cache();
    Env env;
    CrawlHandlerDeps deps{env.deps(), std::nullopt, std::nullopt};
    CHECK(!handle_crawl_message(api, msg("URL なし"), deps));
    CHECK(fd.reactions.empty());

    CHECK(handle_crawl_message(api, msg("[vs] https://store.steampowered.com/app/7/ @everyone"), deps));
    CHECK_EQ(fd.reactions.size(), std::size_t{2});
    CHECK_EQ(fd.reactions[0], std::string("⏳"));
    CHECK_EQ(fd.reactions[1], std::string("✅"));
    CHECK_EQ(fd.messages.size(), std::size_t{1});
    CHECK_EQ(fd.messages[0].first, std::string("sys"));
    const Json& body = fd.messages[0].second;
    CHECK(str::starts_with(body["content"].get<std::string>(), "📥 **データ追加** (計 3 件)"));
    CHECK(str::contains(body["content"].get<std::string>(), "vs ← 3件取込"));
    CHECK(body["allowed_mentions"]["parse"].empty());
}

TEST_CASE("handle_crawl_message: 通知先に送れなければ元メッセージへ返信・取り込み 0 件は ⚠️・DM は返信") {
    FakeDiscord fd;
    DiscordApi api(fd.options());
    clear_system_channel_cache();
    Env env;
    CrawlHandlerDeps deps{env.deps(), std::nullopt, std::nullopt};
    fd.system_send_fails = true;
    CHECK(handle_crawl_message(api, msg("https://youtu.be/abcdef"), deps));
    CHECK_EQ(fd.reactions.back(), std::string("⚠️"));
    CHECK_EQ(fd.messages.size(), std::size_t{1});
    CHECK_EQ(fd.messages[0].first, std::string("crawl"));
    CHECK_EQ(fd.messages[0].second["message_reference"]["message_id"].get<std::string>(), std::string("msg1"));
    CHECK(str::contains(fd.messages[0].second["content"].get<std::string>(), "YouTube API key 未設定"));

    clear_system_channel_cache();
    fd.channels_forbidden = true;
    fd.system_send_fails = false;
    CHECK(handle_crawl_message(api, msg("https://youtu.be/abcdef"), deps));
    CHECK_EQ(fd.messages.back().first, std::string("crawl"));
    CHECK(handle_crawl_message(api, msg("https://youtu.be/abcdef", std::nullopt), deps));
    CHECK_EQ(fd.messages.back().first, std::string("crawl"));
    // 長いまとめは 2000 文字で切る
    std::string many;
    for (int i = 0; i < 20; ++i) many += "https://youtu.be/abcdef" + std::to_string(100000 + i) + str::repeat("x", 60) + " ";
    CHECK(handle_crawl_message(api, msg(many, std::nullopt), deps));
    CHECK(str::js_length(fd.messages.back().second["content"].get<std::string>()) <= kCrawlSummaryMaxChars);
}

DI_TEST_MAIN()
