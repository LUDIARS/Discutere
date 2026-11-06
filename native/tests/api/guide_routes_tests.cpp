// api/guide-routes.ts の移植の検証: Node が返した HTML (golden) と同じ文字列になること・エスケープ。
#include "check.hpp"

#include "api/guide_routes.hpp"
#include "base/crypto.hpp"
#include "base/web_assets.hpp"
#include "w2i_support.hpp"

using namespace di;

namespace {
std::vector<std::string> strings(const Json& a) {
    std::vector<std::string> out;
    for (const auto& v : a) out.push_back(v.get<std::string>());
    return out;
}
} // namespace

TEST_CASE("guide: Node の出力と一致 (config の値・エスケープ・印の注入なし)") {
    for (const auto& c : w2i::load_fixture("guide.json")) {
        const auto& cfg = c["config"];
        api::DiscordConfig d;
        d.forum.enabled = cfg["enabled"].get<bool>();
        d.forum.improvementTagNames = strings(cfg["improvement"]);
        d.forum.funTagNames = strings(cfg["fun"]);
        d.forum.summaryChannelName = cfg["summary"].get<std::string>();
        d.forum.managedCategoryName = cfg["managed"].get<std::string>();
        d.forum.dataLearningChannelName = cfg["learning"].get<std::string>();
        d.gameFeedback.enabled = cfg["fbEnabled"].get<bool>();
        d.gameFeedback.categoryName = cfg["fbCategory"].get<std::string>();
        d.discussionChannelIds = strings(cfg["channels"]);
        const auto html = api::render_guide_html(d);
        if (html != c["html"].get<std::string>()) throw test::Failure("guide html mismatch for " + cfg.dump());
        auto router = api::create_guide_routes(d);
        http::Request q;
        q.method = "GET";
        q.path = "/guide";
        const auto res = router.handle(q);
        CHECK_EQ(res.status, 200);
        CHECK_EQ(res.body, html);
        CHECK_EQ(*res.headers.get("Content-Type"), std::string("text/html; charset=UTF-8"));
    }
    CHECK_EQ(api::guide_escape("<a href=\"x\">&'</a>"), std::string("&lt;a href=&quot;x&quot;&gt;&amp;'&lt;/a&gt;"));
}

TEST_CASE("guide: 雛形は golden 生成の記録と同じ") {
    for (const auto& a : w2i::load_fixture("web_assets.json")) {
        if (a["name"] != "api/guide.html") continue;
        const auto* asset = web::find("api/guide.html");
        CHECK(asset != nullptr);
        CHECK_EQ(crypto::sha256_hex(asset->body), a["sha256"].get<std::string>());
    }
}

DI_TEST_MAIN()
