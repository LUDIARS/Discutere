#include "discord_hook/monitor_card.hpp"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <utility>
#include <vector>

#include "base/log.hpp"
#include "base/strings.hpp"
#include "base/time.hpp"
#include "config.hpp"
#include "core/index.hpp"
#include "core/kg_registry.hpp"
#include "queue/snapshot.hpp"

namespace di::discord_hook {

namespace {

/** snowflake の作成時刻 (createdTimestamp の大小と同じ順)。読めなければ 0。 */
std::uint64_t snowflake_value(const std::string& id) {
    const auto n = str::parse_int(id);
    return n && *n >= 0 ? static_cast<std::uint64_t>(*n) : 0;
}

} // namespace

Json build_monitor_embed(std::int64_t active_sessions, std::int64_t pending_hypotheses, std::int64_t now_ms) {
    const bool active = active_sessions > 0;
    Json embed = Json::object();
    embed["title"] = "🟢 Discutere Monitor";
    embed["color"] = active ? 0x57f287 : 0x99aab5;
    embed["fields"] = Json::array({
        Json{{"name", "状態"}, {"value", active ? "アクティブ" : "アイドル"}, {"inline", true}},
        Json{{"name", "進行中セッション数"}, {"value", std::to_string(active_sessions)}, {"inline", true}},
        Json{{"name", "未処理仮説数"}, {"value", std::to_string(pending_hypotheses)}, {"inline", true}},
    });
    embed["footer"] = Json{{"text", "最終更新"}};
    embed["timestamp"] = time::iso_utc(now_ms);
    return embed;
}

MonitorCard::MonitorCard(MonitorCardDeps deps) : deps_(std::move(deps)) {
    if (!deps_.api) throw std::invalid_argument("monitor card: api required");
}

MonitorCard::~MonitorCard() { timer_.stop(); }

void MonitorCard::start(long long interval_ms) {
    timer_.start(std::chrono::milliseconds(interval_ms), [this] { tick(); },
                 [] { log::warn("  discord-monitor: tick skipped (executor queue full)"); }, true);
}

void MonitorCard::stop() { timer_.stop(); }

// 集計は core の SQLite を都度開いて行う (command-router と同じく使い終わったら閉じる)。
MonitorCard::Counts MonitorCard::counts() {
    core::CoreOptions opts;
    opts.sqlitePath = core::resolve_active_kg_path(config::get_config());
    auto core = core::create_core(opts);
    const auto snap = queue::build_queue_snapshot(core->raw(), nullptr, deps_.workspaceId);
    return {static_cast<std::int64_t>(snap.activeSessions.size()), static_cast<std::int64_t>(snap.pendingHypotheses.size())};
}

std::optional<Channel> MonitorCard::ensure_monitor_channel() {
    std::vector<Channel> channels;
    try {
        channels = deps_.api->get_guild_channels(deps_.guildId);
    } catch (const std::exception&) {
        // TS: guild.channels.fetch().catch(() => null) (キャッシュ無しで続ける)
    }
    for (const auto& c : channels)
        if (c.type == channel_type::GuildText && c.name == std::optional<std::string>(MONITOR_CHANNEL_NAME)) return c;

    std::optional<std::string> category_id;
    for (const auto& c : channels)
        if (c.type == channel_type::GuildCategory && c.name == std::optional<std::string>(STATUS_CATEGORY_NAME)) {
            category_id = c.id;
            break;
        }
    if (!category_id) {
        category_id = deps_.api->create_guild_channel(deps_.guildId, Json{{"name", STATUS_CATEGORY_NAME}, {"type", channel_type::GuildCategory}}).id;
    }
    const auto channel = deps_.api->create_guild_channel(
        deps_.guildId, Json{{"name", MONITOR_CHANNEL_NAME}, {"type", channel_type::GuildText}, {"parent_id", *category_id}});
    if (channel.type != channel_type::GuildText) return std::nullopt;
    return channel;
}

std::optional<std::string> MonitorCard::resolve_existing_monitor_message(const std::string& channel_id) {
    Json recent;
    try {
        RequestOptions ro;
        ro.query.push_back({"limit", std::to_string(MONITOR_FETCH_LIMIT)});
        recent = deps_.api->request("GET", "/channels/" + channel_id + "/messages", std::nullopt, ro);
    } catch (const std::exception&) {
        return std::nullopt;
    }
    if (!recent.is_array()) return std::nullopt;
    const auto me = deps_.api->get_current_user().id;
    std::vector<std::string> cards;
    for (const auto& m : recent) {
        if (!m.is_object()) continue;
        if (json_string(json_object(m, "author"), "id") != me) continue;
        bool is_card = false;
        for (const auto& e : json_array(m, "embeds"))
            if (str::contains(json_string(e, "title"), "Discutere Monitor")) is_card = true;
        const auto id = json_string(m, "id");
        if (is_card && !id.empty()) cards.push_back(id);
    }
    std::stable_sort(cards.begin(), cards.end(),
                     [](const std::string& a, const std::string& b) { return snowflake_value(b) < snowflake_value(a); });
    if (cards.empty()) return std::nullopt;
    for (std::size_t i = 1; i < cards.size(); ++i) {
        try {
            deps_.api->request("DELETE", "/channels/" + channel_id + "/messages/" + cards[i]);
        } catch (const std::exception&) {
            // 重複カードの削除失敗は無視
        }
    }
    return cards.front();
}

void MonitorCard::tick() {
    std::lock_guard lock(mutex_);
    try {
        try {
            deps_.api->get_guild(deps_.guildId);
        } catch (const std::exception&) {
            return;  // guild に居ない・取得失敗
        }
        const auto channel = ensure_monitor_channel();
        if (!channel) return;
        const auto c = counts();
        const Json body{{"embeds", Json::array({build_monitor_embed(c.activeSessions, c.pendingHypotheses, time::now_ms())})}};

        if (!message_id_) message_id_ = resolve_existing_monitor_message(channel->id);
        if (message_id_) {
            bool exists = true;
            try {
                deps_.api->get_message(channel->id, *message_id_);
            } catch (const std::exception&) {
                exists = false;
            }
            if (exists) {
                deps_.api->edit_message(channel->id, *message_id_, body);
                return;
            }
        }
        message_id_ = deps_.api->create_message(channel->id, body).id;
    } catch (const std::exception& err) {
        log::warn(std::string("  discord-monitor: tick failed: ") + err.what());
    }
}

} // namespace di::discord_hook
