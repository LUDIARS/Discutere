#include "api/guide_routes.hpp"

#include <map>
#include <memory>

#include "api/web_page.hpp"
#include "api/web_template.hpp"
#include "base/strings.hpp"

namespace di::api {

namespace {

/** タグ名配列を `<code>A</code> <code>B</code>` の HTML に整形する。 */
std::string tag_pills(const std::vector<std::string>& names) {
    std::vector<std::string> parts;
    parts.reserve(names.size());
    for (const auto& n : names) parts.push_back("<code>" + guide_escape(n) + "</code>");
    return str::join(parts, " ");
}

} // namespace

std::string guide_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        default: out += c;
        }
    }
    return out;
}

std::string render_guide_html(const DiscordConfig& discord) {
    const auto& f = discord.forum;
    const auto& fb = discord.gameFeedback;
    std::size_t plain_channel_count = 0;
    for (const auto& id : discord.discussionChannelIds)
        if (!id.empty()) ++plain_channel_count;
    const std::map<std::string, std::string> values = {
        {"forumEnabled", f.enabled ? "ON" : "OFF"},
        {"improvementTagPills", tag_pills(f.improvementTagNames)},
        {"funTagPills", tag_pills(f.funTagNames)},
        {"summaryChannelName", guide_escape(f.summaryChannelName)},
        {"managedCategoryName", guide_escape(f.managedCategoryName)},
        {"dataLearningChannelName", guide_escape(f.dataLearningChannelName)},
        {"gameFeedbackDisabled", fb.enabled ? "" : " — 無効"},
        {"gameFeedbackCategoryName", guide_escape(fb.categoryName)},
        {"plainChannelCount", std::to_string(plain_channel_count)},
    };
    return fill_template(web_page("api/guide.html"), values);
}

http::Router create_guide_routes(const DiscordConfig& discord) {
    auto html = std::make_shared<const std::string>(render_guide_html(discord));
    http::Router router;
    router.get("/guide", [html](http::Request&) { return http::Response::html(*html); });
    return router;
}

} // namespace di::api
