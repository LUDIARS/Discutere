#include "discord_hook/forum_monitor.hpp"

#include <map>
#include <memory>
#include <thread>

#include "base/log.hpp"
#include "base/strings.hpp"
#include "discord_hook/normalize.hpp"
#include "discord_hook/system_channel.hpp"

namespace di::discord_hook {

const ForumDirectionConfig& DEFAULT_FORUM_DIRECTION_CONFIG() {
    static const ForumDirectionConfig cfg{{"改善提案", "改善", "提案", "improvement"}, {"面白さ", "面白い", "おもしろさ", "fun"}};
    return cfg;
}

ForumDirection pick_forum_direction(const std::vector<std::string>& applied_tag_names, const ForumDirectionConfig& cfg) {
    std::vector<std::string> names;
    for (const auto& n : applied_tag_names) {
        auto t = str::to_lower(str::trim(n));
        if (!t.empty()) names.push_back(std::move(t));
    }
    const auto matches = [&](const std::vector<std::string>& cands) {
        for (const auto& n : names)
            for (const auto& c : cands) {
                const auto cc = str::to_lower(str::trim(c));
                if (!cc.empty() && (n == cc || str::contains(n, cc))) return true;
            }
        return false;
    };
    if (matches(cfg.improvementTagNames)) return "improvement";
    if (matches(cfg.funTagNames)) return "fun";
    return DEFAULT_FORUM_DIRECTION;
}

std::optional<DiscordScene> parse_discord_scene(const std::optional<std::string>& scene) {
    if (!scene || !str::starts_with(*scene, "discord:")) return std::nullopt;
    const auto rest = scene->substr(std::string_view("discord:").size());
    const auto slash = rest.find('/');
    if (slash == std::string::npos || slash == 0) return std::nullopt;
    DiscordScene out{rest.substr(0, slash), rest.substr(slash + 1)};
    if (out.guildId.empty() || out.channelId.empty()) return std::nullopt;
    return out;
}

bool is_forum_thread_channel(const ForumChannelRef* channel) {
    if (!channel || !channel->is_thread) return false;
    return channel->parent && channel->parent->type && *channel->parent->type == channel_type::GuildForum;
}

bool is_forum_starter_message(const std::string& message_id, const std::string& channel_id) { return message_id == channel_id; }

std::string format_forum_summary(const std::string& title, const std::string& summary) {
    auto body = str::trim(summary);
    if (body.empty()) body = "(まとめ未生成)";
    return "✅ **収束**: " + title + "\n" + body;
}

std::vector<std::string> thread_applied_tag_names(const Channel& thread, const std::optional<Channel>& parent_forum) {
    if (thread.applied_tags.empty()) return {};
    std::map<std::string, std::string> by_id;
    if (parent_forum)
        for (const auto& t : parent_forum->available_tags)
            if (t.id) by_id.emplace(*t.id, t.name);
    std::vector<std::string> out;
    for (const auto& id : thread.applied_tags) {
        const auto it = by_id.find(id);
        if (it != by_id.end() && !it->second.empty()) out.push_back(it->second);
    }
    return out;
}

namespace {

void sleep_for(const ForumMonitorDeps& deps, std::chrono::milliseconds ms) {
    if (deps.sleep)
        deps.sleep(ms);
    else
        std::this_thread::sleep_for(ms);
}

/** starter (= スレッド id と同じ id のメッセージ) を取得する。作成直後は未確定なことがあるので 1 回リトライ。 */
std::optional<Json> fetch_starter_with_retry(DiscordApi& api, const std::string& thread_id, const ForumMonitorDeps& deps) {
    for (int attempt = 0; attempt < 2; ++attempt) {
        try {
            auto m = api.request("GET", "/channels/" + thread_id + "/messages/" + thread_id);
            if (m.is_object()) return m;
        } catch (const std::exception&) {
            /* 作成直後は未確定なことがある → リトライ */
        }
        sleep_for(deps, std::chrono::milliseconds(500));
    }
    return std::nullopt;
}

} // namespace

void handle_forum_thread_create(const Json& thread_json, DiscordApi& api, const ForumMonitorDeps& deps) {
    Channel thread;
    try {
        thread = channel_from_json(thread_json);
    } catch (const std::exception&) {
        return;
    }
    // 親は REST で取得して種別 + タグ定義を得る (取得不可なら判定不能 → skip)。
    if (!thread.parent_id) return;
    std::optional<Channel> parent;
    try {
        parent = api.get_channel(*thread.parent_id);
    } catch (const std::exception&) {
        return;
    }
    if (parent->type != channel_type::GuildForum) return;
    const auto guild_id = thread.guild_id ? thread.guild_id : parent->guild_id;
    if (!guild_id || guild_id->empty()) return;

    // 適用タグから議論の方向性を決める (タグ無し → 既定 = 面白さ)。
    // タグ名自体は結論まとめの絞り込み用に evidence へも持ち回す。
    const auto applied = thread_applied_tag_names(thread, parent);
    const auto direction = pick_forum_direction(applied, deps.directionConfig ? *deps.directionConfig : DEFAULT_FORUM_DIRECTION_CONFIG());

    auto starter = fetch_starter_with_retry(api, thread.id, deps);
    if (!starter) {
        log::warn("  discord-forum: starter message 取得不可 (thread=" + thread.id + ")");
        return;
    }
    if (starter->contains("author") && (*starter)["author"].is_object() && json_bool((*starter)["author"], "bot")) return;

    // session/scene を thread に紐付ける。
    Json raw = *starter;
    raw["channel_id"] = thread.id;
    const auto normalized = normalize_discord_inbound_message(raw);
    if (!normalized) return;

    try {
        ForumPostOptions opts;
        opts.isStarter = true;
        opts.direction = direction;
        // スレッド名 = 議論の主題。本文にゲーム名が無くても議題を固定する。
        opts.forumTitle = thread.name;
        // 適用タグ名 → evidence.tags (結論まとめの絞り込み用)。
        opts.forumTags = applied;
        const auto res = route_forum_post(*normalized, *guild_id, deps.router, opts);
        if (res.ingested && res.seed && res.seed()) {
            try {
                api.create_reaction(thread.id, normalized->id, "👀");
            } catch (const std::exception&) {
            }
        }
    } catch (const std::exception& e) {
        log::warn(std::string("  discord-forum: thread-create route failed: ") + e.what());
    }
}

void handle_forum_reply(const Json& message, const ForumMonitorDeps& deps) {
    const auto guild_id = message.is_object() ? json_opt_string(message, "guild_id") : std::nullopt;
    if (!guild_id || guild_id->empty()) return;
    const auto normalized = normalize_discord_inbound_message(message);
    if (!normalized) return;
    try {
        ForumPostOptions opts;
        opts.isStarter = false;
        route_forum_post(*normalized, *guild_id, deps.router, opts);
    } catch (const std::exception& e) {
        log::warn(std::string("  discord-forum: reply route failed: ") + e.what());
    }
}

FinalizeForumPostResult finalize_forum_post(DiscordApi& api, const FinalizeForumPostArgs& args) {
    const auto parsed = parse_discord_scene(args.scene);
    if (!parsed) return {false, "scene is not discord-bound"};

    Channel channel;
    try {
        channel = api.get_channel(parsed->channelId);
    } catch (const std::exception& e) {
        return {false, std::string("channel fetch failed: ") + e.what()};
    }
    // 親の種別を確かめる (discord.js の channel.parent キャッシュの代わりに REST で取る)。
    auto ref = forum_ref_from_channel(channel);
    if (channel.is_thread() && channel.parent_id) {
        try {
            ref.parent = std::make_shared<const ForumChannelRef>(forum_ref_from_channel(api.get_channel(*channel.parent_id)));
        } catch (const std::exception&) {
            /* 親が取れなければフォーラムと判定できない */
        }
    }
    if (!is_forum_thread_channel(&ref)) return {false, "not a forum thread"};

    // 1) まとめを「まとめ投稿」チャンネルへ転記 (権限が無ければ skip)。
    try {
        EnsureSystemChannelOptions o;
        o.guildId = parsed->guildId;
        o.categoryName = args.categoryName;
        o.channelName = args.summaryChannelName.value_or("まとめ投稿");
        o.topic = "収束した議論の結論まとめ (Discutere フォーラム議論)";
        if (const auto summary_channel = ensure_system_channel(api, o)) {
            const auto link = "\nhttps://discord.com/channels/" + parsed->guildId + "/" + channel.id;
            Json body = Json::object();
            body["content"] = format_forum_summary(args.title, args.summary) + link;
            body["allowed_mentions"] = allowed_mentions_none();
            try {
                api.create_message(summary_channel->id, body);
            } catch (const std::exception&) {
            }
        }
    } catch (const std::exception&) {
        /* まとめ転記の失敗はクローズを止めない */
    }

    // 2) ポストをクローズ (lock → archive)。lock は権限が無ければ失敗するので無視する (TS の manageable 判定の代わり)。
    try {
        api.modify_channel(channel.id, Json{{"locked", true}}, std::string("discussion converged"));
    } catch (const std::exception&) {
    }
    try {
        api.modify_channel(channel.id, Json{{"archived", true}}, std::string("discussion converged"));
        return {true, std::nullopt};
    } catch (const std::exception& e) {
        return {false, std::string("archive failed: ") + e.what()};
    }
}

} // namespace di::discord_hook
