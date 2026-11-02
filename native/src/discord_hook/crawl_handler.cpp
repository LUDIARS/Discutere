#include "discord_hook/crawl_handler.hpp"

#include "base/strings.hpp"
#include "discord_hook/system_channel.hpp"

namespace di::discord_hook {

namespace {

void safe_react(DiscordApi& api, const Message& msg, const std::string& emoji) {
    try {
        api.create_reaction(msg.channel_id, msg.id, emoji);
    } catch (...) {
        /* リアクション権限が無くても処理は続行 */
    }
}

/** まとめは利用者の URL と取得先のエラー文を含むので一括メンションを解釈させない (移植で追加)。 */
Json post_body(const std::string& content) {
    return Json{{"content", str::js_slice(content, 0, static_cast<std::int64_t>(kCrawlSummaryMaxChars))}, {"allowed_mentions", allowed_mentions_none()}};
}

} // namespace

bool handle_crawl_message(DiscordApi& api, const Message& msg, const CrawlHandlerDeps& deps) {
    const auto parsed = parse_crawl_message(msg.content);
    if (parsed.urls.empty()) return false;

    safe_react(api, msg, "⏳");
    std::string summary;
    bool ok = false;
    try {
        const auto results = ingest_crawl_urls(deps.crawl, CrawlIngestInput{parsed.urls, parsed.gameSlug});
        for (const auto& r : results) ok = ok || r.imported > 0;
        summary = format_crawl_summary(results);
    } catch (const std::exception& err) {
        summary = std::string("⚠️ クロール失敗: ") + err.what();
    }

    safe_react(api, msg, ok ? "✅" : "⚠️");

    // 「データ追加」チャンネルへ通知 (guild 内のみ。無ければ作成、権限無ければ元チャンネルに返す)。
    bool posted = false;
    if (msg.guild_id && !msg.guild_id->empty()) {
        EnsureSystemChannelOptions o;
        o.guildId = *msg.guild_id;
        o.categoryName = deps.categoryName;
        o.channelName = deps.channelName;
        std::optional<Channel> channel;
        try {
            channel = ensure_system_channel(api, o);
        } catch (...) {
        }
        if (channel) {
            try {
                api.create_message(channel->id, post_body(summary));
                posted = true;
            } catch (...) {
                /* 送信失敗 → 元チャンネルにフォールバック */
            }
        }
    }
    if (!posted) {
        try {
            Json body = post_body(summary);
            body["message_reference"] = Json{{"message_id", msg.id}};
            body["allowed_mentions"]["replied_user"] = true;
            api.create_message(msg.channel_id, body);
        } catch (...) {
            /* 返信も不可なら諦める (リアクションは付いている) */
        }
    }
    return true;
}

} // namespace di::discord_hook
