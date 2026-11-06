#include "discord_hook/command_router.hpp"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <stdexcept>

#include "base/executor.hpp"
#include "base/json.hpp"
#include "base/log.hpp"
#include "base/strings.hpp"
#include "base/time.hpp"
#include "config.hpp"
#include "core/kg_registry.hpp"
#include "core/projection/message_input.hpp"
#include "discord_hook/normalize.hpp"
#include "discord_hook/slash_interaction.hpp"
#include "flow/discussion_paper.hpp"
#include "visualize/conclusions.hpp"

namespace di::discord_hook {

namespace {

bool contains(const std::vector<std::string>& list, const std::string& v) { return std::find(list.begin(), list.end(), v) != list.end(); }

std::unique_ptr<core::Core> open_core(const CommandRouterDeps& deps) {
    if (deps.openCore) return deps.openCore();
    core::CoreOptions opts;
    opts.sqlitePath = core::resolve_active_kg_path(config::get_config());
    return core::create_core(opts);
}

bool submit_background(const CommandRouterDeps& deps, std::function<void()> task) {
    if (deps.submitBackground) return deps.submitBackground(std::move(task));
    return background_executor().submit(std::move(task));
}

/** core を閉じる (例外の途中でも)。 */
struct CoreCloser {
    core::Core* core;
    ~CoreCloser() {
        try {
            core->close();
        } catch (...) {
        }
    }
};

std::string ensure_discord_session(core::Core& core, const std::string& workspace_id, const std::string& guild_id,
                                   const std::string& channel_id) {
    const std::string title = "discord-session:" + guild_id + ":" + channel_id;
    const auto existing =
        core.raw().get("SELECT id FROM sessions WHERE workspace_id = ? AND title = ? ORDER BY started_at DESC LIMIT 1", {workspace_id, title});
    if (existing) return existing->str("id");
    core::repositories::SessionInput s;
    s.workspaceId = workspace_id;
    s.title = title;
    s.startedAt = static_cast<double>(time::now_ms());
    s.scene = "discord:" + guild_id + "/" + channel_id;
    return core.repos.session.create(s);
}

/** 当該スレッド (scene=discord:<guild>/<threadId>) で open な議論 session が既にあるか。 */
bool forum_discussion_exists(core::Core& core, const std::string& workspace_id, const std::string& guild_id, const std::string& thread_id) {
    const std::string scene = "discord:" + guild_id + "/" + thread_id;
    const auto row = core.raw().get(
        R"(SELECT s.id AS id
         FROM sessions s
         JOIN design_gaps g
           ON g.id = SUBSTR(s.title, LENGTH('discussion-of-gap:') + 1)
        WHERE s.workspace_id = ?
          AND s.title LIKE 'discussion-of-gap:%'
          AND s.scene = ?
          AND (g.status IS NULL OR g.status NOT IN ('closed', 'converged', 'dismissed'))
        LIMIT 1)",
        {workspace_id, scene});
    return row.has_value();
}

/** 分類器の呼び出しを SeedTask に包む (例外は warn して false。TS の .catch)。 */
SeedTask make_seed(const ClassifyInboundMessage& classify, DiscordAutoDiscussionInput input, std::string warn_prefix) {
    return [classify, input = std::move(input), warn_prefix = std::move(warn_prefix)]() -> bool {
        try {
            return classify(input).started;
        } catch (const std::exception& e) {
            log::warn(warn_prefix + e.what());
            return false;
        }
    };
}

/** admin allowlist の判定 (未設定は全拒否)。通ってよければ nullopt。 */
std::optional<SlashReply> admin_guard(const InboundSlashCommand& cmd, const CommandRouterDeps& deps, const char* unset_message) {
    if (deps.adminIds.empty()) return SlashReply{unset_message, true};
    if (!cmd.userId || !contains(deps.adminIds, *cmd.userId)) return SlashReply{"⚠️ admin only", true};
    return std::nullopt;
}

constexpr const char* kAdminUnsetJa = "⚠️ discord.adminIds 未設定 — admin コマンドは無効";

/** /discutere-queue — 議論キューのサマリを ephemeral で返す (全員可) */
SlashReply handle_queue_slash(const CommandRouterDeps& deps) {
    if (!deps.buildQueueText) return {"⚠️ queue view が利用できません (起動構成を確認)", true};
    try {
        return {deps.buildQueueText(), true};
    } catch (const std::exception& e) {
        return {std::string("⚠️ queue 取得失敗: ") + e.what(), true};
    }
}

/**
 * /discutere-backup — 手動 S3 バックアップ (admin only)。
 * tar+upload は数秒かかり得るので背景で起動し、即時 ack を返す (Discord interaction の 3 秒 ack 制限回避)。
 */
SlashReply handle_backup_slash(const InboundSlashCommand& cmd, const CommandRouterDeps& deps) {
    if (auto g = admin_guard(cmd, deps, kAdminUnsetJa)) return *g;
    if (!deps.triggerBackup) return {"⚠️ backup が構成されていません (backup.bucket 未設定?)", true};
    auto trigger = deps.triggerBackup;
    const bool queued = submit_background(deps, [trigger] {
        try {
            const auto r = trigger();
            if (r.ok)
                log::info("  backup(manual): uploaded s3://" + r.bucket.value_or("undefined") + "/" + r.key.value_or("undefined"));
            else
                log::warn("  backup(manual): failed: " + r.error.value_or("undefined"));
        } catch (const std::exception& e) {
            log::warn(std::string("  backup(manual): error: ") + e.what());
        }
    });
    if (!queued) return {"⚠️ バックアップを開始できませんでした (処理待ちが満杯です。時間をおいて再実行してください)", true};
    return {"🗄️ バックアップを開始しました (完了はサーバログ / dashboard で確認)", true};
}

std::string format_conclusion_list(const std::vector<visualize::ConclusionSummary>& list) {
    if (list.empty()) return "まだ収束した議論はありません。";
    std::vector<std::string> lines{"【結論一覧 (新しい順 " + std::to_string(list.size()) + "件)】"};
    for (const auto& c : list) {
        const auto head = c.conclusion && !c.conclusion->empty() ? truncate_text(*c.conclusion, 120) : std::string("(まとめ未生成)");
        lines.push_back("• **" + c.title + "**\n  " + head + "\n  └ 詳細: /discutere-conclusions gap:" + c.gapId + " (発言" +
                        std::to_string(c.utteranceCount) + "/止揚" + std::to_string(c.aufhebungCount) + ")");
    }
    return str::join(lines, "\n");
}

std::string format_conclusion_detail(const visualize::ConclusionDetail& d) {
    std::vector<std::string> parts{"【結論】" + d.title};
    parts.push_back(d.conclusion && !d.conclusion->empty() ? *d.conclusion : "(まとめ未生成)");
    if (!d.aufhebungen.empty()) {
        parts.push_back("\n── 止揚ストック ──");
        std::vector<std::string> items;
        for (std::size_t i = 0; i < d.aufhebungen.size(); ++i) items.push_back(std::to_string(i + 1) + ". " + d.aufhebungen[i]);
        parts.push_back(str::join(items, "\n"));
    }
    if (!d.topOpinions.empty()) {
        parts.push_back("\n── 高評価意見 ──");
        std::vector<std::string> items;
        for (const auto& o : d.topOpinions) items.push_back("+" + std::to_string(o.score) + " " + o.speaker + ": " + truncate_text(o.content, 100));
        parts.push_back(str::join(items, "\n"));
    }
    parts.push_back("\n── 議論ログ (" + std::to_string(d.transcript.size()) + "発言) ──");
    std::vector<std::string> items;
    for (const auto& u : d.transcript) items.push_back("[" + u.speaker + "] " + truncate_text(u.content, 120));
    parts.push_back(str::join(items, "\n"));
    return truncate_text(str::join(parts, "\n"), 1900);  // Discord 2000 字制限に収める
}

/**
 * /discutere-conclusions — 収束した議論の結論を一覧 / gap 指定で論述データを表示 (#66)。
 * 認可不要 (読み取り専用)。ephemeral で返す。
 */
SlashReply handle_conclusions_slash(const InboundSlashCommand& cmd, const CommandRouterDeps& deps) {
    try {
        auto core = open_core(deps);
        CoreCloser closer{core.get()};
        const auto gap_id = str::trim(cmd.argsText);
        if (!gap_id.empty()) {
            const auto detail = visualize::get_conclusion_detail(*core, deps.workspaceId, gap_id);
            if (!detail) return {"⚠️ 結論が見つかりません (gap=" + gap_id + ")", true};
            return {format_conclusion_detail(*detail), true};
        }
        return {format_conclusion_list(visualize::list_conclusions(*core, deps.workspaceId, 10)), true};
    } catch (const std::exception& e) {
        return {std::string("⚠️ 結論取得失敗: ") + e.what(), true};
    }
}

std::string discussion_badge(const flow::FlowSessionRow& r) {
    if (r.status == "draft") return "📝下書き";
    if (r.concluded && *r.concluded == 1) return "✅結論あり";
    return "💬進行中";
}

std::string format_discussion_list(const std::vector<flow::FlowSessionRow>& rows, std::int64_t total, const std::string& state) {
    const std::string label = state == "all" ? "全件" : state;
    if (rows.empty()) return "該当する議論はありません (" + label + ")。";
    std::vector<std::string> lines{"【議論一覧 (" + label + "・新しい順 " + std::to_string(rows.size()) + "/" + std::to_string(total) + "件)】"};
    for (const auto& r : rows) {
        const auto when = ja_locale_string(static_cast<double>(r.createdAt));
        const auto no = r.discussionNo && *r.discussionNo != 0 ? "#" + std::to_string(*r.discussionNo) : std::string("#?");
        lines.push_back("• " + discussion_badge(r) + " " + no + " **" + truncate_text(r.theme.empty() ? "(無題)" : r.theme, 60) + "**\n  └ " +
                        r.flow + " / 発話" + std::to_string(r.utterances) + " / " + when);
    }
    const auto more = total > static_cast<std::int64_t>(rows.size())
                          ? "\n…他 " + std::to_string(total - static_cast<std::int64_t>(rows.size())) + " 件"
                          : std::string();
    return str::join(lines, "\n") + more;
}

/**
 * /discutere-discussions — 議論一覧 (下書き/進行中/結論あり) を ephemeral で返す (Web の議論一覧と対応)。
 * state option で絞り込み (省略で全件)。認可不要 (読み取り専用)。
 */
SlashReply handle_discussions_slash(const InboundSlashCommand& cmd) {
    const auto raw = str::trim(cmd.argsText);
    const std::string state = raw == "draft" || raw == "live" || raw == "concluded" ? raw : "all";
    try {
        flow::ListFlowSessionsOptions o;
        o.state = state;
        o.limit = 15;
        o.offset = 0;
        const auto res = flow::list_flow_sessions(o);
        return {format_discussion_list(res.rows, res.total, state), true};
    } catch (const std::exception& e) {
        return {std::string("⚠️ 議論一覧の取得に失敗: ") + e.what(), true};
    }
}

/** /economy-graph — LLM 分析を fire-and-forget で起動してグラフ URL を返す。 */
SlashReply handle_economy_graph_slash(const InboundSlashCommand& cmd, const CommandRouterDeps& deps) {
    const auto game_title = str::trim(cmd.argsText);
    if (game_title.empty()) return {"⚠️ game オプションにゲームタイトルを指定してください", true};
    if (!deps.triggerEconomyAnalysis) return {"⚠️ economy-graph が構成されていません (ANTHROPIC_API_KEY 未設定?)", true};
    const auto started = deps.triggerEconomyAnalysis(game_title);
    const auto url = "http://localhost:" + std::to_string(started.port) + "/ludus/economy-graph/" + started.slug;
    return {"🎮 **" + game_title + "** の経済分析を開始しました。\n📊 グラフ: " + url + "\n_分析完了後にリロードしてください (約 30 秒)_", true};
}

/**
 * /persona-generate — 憑依対象ペルソナを外部データから生成 (admin only, item7)。
 * KG 集約 → C1 採用は数秒かかり得るので背景で起動し即時 ack。結果はサーバログ。
 */
SlashReply handle_persona_generate_slash(const InboundSlashCommand& cmd, const CommandRouterDeps& deps) {
    if (auto g = admin_guard(cmd, deps, kAdminUnsetJa)) return *g;
    if (!deps.triggerPersonaGenerate) return {"⚠️ ペルソナ生成が構成されていません", true};
    const auto trimmed = str::trim(cmd.argsText);
    const std::optional<std::string> source = trimmed.empty() ? std::nullopt : std::optional<std::string>(trimmed);
    auto trigger = deps.triggerPersonaGenerate;
    const bool queued = submit_background(deps, [trigger, source] {
        try {
            const auto r = trigger(source);
            log::info("  persona-generate(manual): " + js_number_to_string(r.adopted) + " 採用 / " + js_number_to_string(r.speakers) +
                      " 話者 (source=" + source.value_or("all") + ")");
        } catch (const std::exception& e) {
            log::warn(std::string("  persona-generate(manual): error: ") + e.what());
        }
    });
    if (!queued) return {"⚠️ ペルソナ生成を開始できませんでした (処理待ちが満杯です。時間をおいて再実行してください)", true};
    return {"🧬 憑依対象ペルソナの生成を開始しました" + (source ? " (source=" + *source + ")" : std::string()) + " (完了はサーバログで確認)", true};
}

/**
 * /discutere-kill / /discutere-status の handler。
 * 認可: deps.adminIds allowlist。未設定なら全 reject (安全 default、設定必須)。
 */
SlashReply handle_engine_slash(const InboundSlashCommand& cmd, const CommandRouterDeps& deps) {
    if (auto g = admin_guard(cmd, deps, "⚠️ discord.adminIds not set — admin commands are disabled")) return *g;
    const auto engine = deps.getEngine ? deps.getEngine() : nullptr;
    if (!engine) return {"⚠️ persona-engine not initialized", true};

    if (cmd.name == "discutere-status") {
        const auto personas = engine->personas().list().size();
        persona_engine::db::RuleListFilter f;
        f.enabled = true;
        const auto rules = engine->rules().list(f).size();
        return {"🤖 persona-engine\n personas: `" + std::to_string(personas) + "`\n active rules: `" + std::to_string(rules) + "`", true};
    }

    // discutere-kill
    if (!cmd.enabledOption) return {"usage: /discutere-kill enabled:true|false", true};
    engine->set_rules_enabled(*cmd.enabledOption);
    return {*cmd.enabledOption ? "✅ persona-engine **ENABLED** (rules running)" : "🛑 persona-engine **DISABLED** (rules paused)", true};
}

} // namespace

// ─── 整形 ────────────────────────────────────────────────────────────

std::string truncate_text(std::string_view value, std::int64_t max) {
    // value.replace(/\n+/g, m => m.length > 1 ? "\n" : m) — 連続改行を 1 つに。
    std::string flat;
    flat.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '\n' && i > 0 && value[i - 1] == '\n') continue;
        flat.push_back(value[i]);
    }
    if (static_cast<std::int64_t>(str::js_length(flat)) <= max) return flat;
    return str::js_slice(flat, 0, max - 1) + "…";
}

std::string ja_locale_string(double ms) {
    if (!(ms == ms) || ms > 8.64e15 || ms < -8.64e15) return "Invalid Date";
    const auto secs = static_cast<std::time_t>(std::floor(ms / 1000.0));
    std::tm tm{};
#ifdef _WIN32
    if (localtime_s(&tm, &secs) != 0) return "Invalid Date";
#else
    if (!localtime_r(&secs, &tm)) return "Invalid Date";
#endif
    auto two = [](int v) { return v < 10 ? "0" + std::to_string(v) : std::to_string(v); };
    return std::to_string(tm.tm_year + 1900) + "/" + std::to_string(tm.tm_mon + 1) + "/" + std::to_string(tm.tm_mday) + " " +
           std::to_string(tm.tm_hour) + ":" + two(tm.tm_min) + ":" + two(tm.tm_sec);
}

// ─── 登録との一致 ─────────────────────────────────────────────────────

const std::vector<std::string>& ROUTER_SLASH_COMMANDS() {
    static const std::vector<std::string> v{"discutere-kill",        "discutere-status", "discutere-queue", "discutere-backup",
                                            "discutere-conclusions", "discutere-discussions", "economy-graph", "persona-generate"};
    return v;
}

const std::vector<std::string>& DEDICATED_SLASH_COMMANDS() {
    static const std::vector<std::string> v{"mechanics-check", "level-check", "discutere-paper-gap", "debate"};
    return v;
}

const std::vector<std::string>& DISCATIER_COMMANDS() {
    static const std::vector<std::string> v{"define", "intends",   "refine", "compare",   "history", "scene", "done",
                                            "refs",   "propose",   "addresses", "jump",   "submit",  "validate", "integrate",
                                            "reject", "me",        "find",   "lineage",   "cluster", "stale", "hotspot"};
    return v;
}

const std::vector<std::string>& ADMIN_SLASH_COMMANDS() {
    static const std::vector<std::string> v{"discutere-kill", "discutere-status", "discutere-backup", "persona-generate"};
    return v;
}

// ─── ルーティング ─────────────────────────────────────────────────────

SlashReply route_slash_command(const InboundSlashCommand& cmd, const CommandRouterDeps& deps) {
    // Even an incorrectly routed diagnostic must never become a persisted utterance.
    if (cmd.name == "mechanics-check" || cmd.name == "level-check") return {cmd.name + " requires its dedicated async handler", true};
    if (cmd.name == "discutere-kill" || cmd.name == "discutere-status") return handle_engine_slash(cmd, deps);
    if (cmd.name == "discutere-queue") return handle_queue_slash(deps);
    if (cmd.name == "discutere-backup") return handle_backup_slash(cmd, deps);
    if (cmd.name == "discutere-conclusions") return handle_conclusions_slash(cmd, deps);
    if (cmd.name == "discutere-discussions") return handle_discussions_slash(cmd);
    if (cmd.name == "economy-graph") return handle_economy_graph_slash(cmd, deps);
    if (cmd.name == "persona-generate") return handle_persona_generate_slash(cmd, deps);

    const auto command_text = cmd.argsText.empty() ? "/" + cmd.name : "/" + cmd.name + " " + cmd.argsText;
    auto core = open_core(deps);
    CoreCloser closer{core.get()};
    const auto session_id = ensure_discord_session(*core, deps.workspaceId, cmd.guildId, cmd.channelId);
    core::projection::SubmitMessageInput in;
    in.core = core.get();
    in.workspaceId = deps.workspaceId;
    in.sessionId = session_id;
    in.rawContent = command_text;
    const auto res = core::projection::submit_message(in);
    if (!res.commandResult) return {"message routed", true};
    const auto& r = *res.commandResult;
    return {r.ok ? "✅ " + r.message.value_or("ok") : "⚠️ " + r.error.value_or("command failed"), !r.ok};
}

InboundRouteResult route_inbound_message(const DiscordInboundMessage& msg, const std::string& guild_id, const CommandRouterDeps& deps,
                                         const std::optional<std::string>& parent_channel_id) {
    // 議論チャンネル (または親が議論チャンネルのスレッド) のみ取り込む。
    const bool allowed = contains(deps.discussionChannelIds, msg.channelId) ||
                         (parent_channel_id && !parent_channel_id->empty() && contains(deps.discussionChannelIds, *parent_channel_id));
    if (!allowed) return {};
    const auto text = str::trim(msg.content);
    if (text.empty()) return {};
    // slash 由来 (先頭 "/") は interaction 経路で処理されるので二重取り込みしない。
    if (str::starts_with(text, "/")) return {};

    auto core = open_core(deps);
    CoreCloser closer{core.get()};
    const auto session_id = ensure_discord_session(*core, deps.workspaceId, guild_id, msg.channelId);
    core::projection::SubmitMessageInput in;
    in.core = core.get();
    in.workspaceId = deps.workspaceId;
    in.sessionId = session_id;
    in.personId = msg.author.id;
    in.rawContent = text;
    const auto res = core::projection::submit_message(in);
    InboundRouteResult out;
    out.ingested = true;
    if (!res.utteranceId.empty() && deps.classifyInboundMessage) {
        DiscordAutoDiscussionInput input;
        input.workspaceId = deps.workspaceId;
        input.guildId = guild_id;
        input.channelId = msg.channelId;
        input.sessionId = session_id;
        input.utteranceId = res.utteranceId;
        input.authorId = msg.author.id;
        input.content = text;
        out.seed = make_seed(deps.classifyInboundMessage, std::move(input), "  discord-auto-discussion: failed: ");
    }
    return out;
}

InboundRouteResult route_forum_post(const DiscordInboundMessage& msg, const std::string& guild_id, const CommandRouterDeps& deps,
                                    const ForumPostOptions& opts) {
    const auto text = str::trim(msg.content);
    if (text.empty()) return {};
    // slash 由来 (先頭 "/") は interaction 経路で処理されるので二重取り込みしない。
    if (str::starts_with(text, "/")) return {};

    auto core = open_core(deps);
    CoreCloser closer{core.get()};
    const auto session_id = ensure_discord_session(*core, deps.workspaceId, guild_id, msg.channelId);
    core::projection::SubmitMessageInput in;
    in.core = core.get();
    in.workspaceId = deps.workspaceId;
    in.sessionId = session_id;
    in.personId = msg.author.id;
    in.rawContent = text;
    const auto res = core::projection::submit_message(in);

    // starter のみ議論を起こす。既に当該スレッドで open な議論があれば二重起動しない。
    const bool should_seed = opts.isStarter && !res.utteranceId.empty() && deps.classifyInboundMessage &&
                             !forum_discussion_exists(*core, deps.workspaceId, guild_id, msg.channelId);
    InboundRouteResult out;
    out.ingested = true;
    if (!should_seed) return out;
    DiscordAutoDiscussionInput input;
    input.workspaceId = deps.workspaceId;
    input.guildId = guild_id;
    input.channelId = msg.channelId;
    input.sessionId = session_id;
    input.utteranceId = res.utteranceId;
    input.authorId = msg.author.id;
    input.content = text;
    input.forumTitle = opts.forumTitle;
    input.direction = opts.direction;
    input.forumTags = opts.forumTags;
    out.seed = make_seed(deps.classifyInboundMessage, std::move(input), "  discord-forum: classify failed: ");
    return out;
}

// ─── 入口 ─────────────────────────────────────────────────────────────

std::string stringify_option_value(const Json& value) {
    if (value.is_string()) return value.get<std::string>();
    if (value.is_null()) return "";
    return json_stringify(value);
}

InboundSlashCommand inbound_slash_command_from_json(const Json& interaction) {
    const auto s = slash_interaction_from_json(interaction);
    InboundSlashCommand cmd;
    cmd.name = s.commandName;
    std::vector<std::string> parts;
    for (const auto& o : s.options) {
        auto v = stringify_option_value(o.value);
        if (!v.empty()) parts.push_back(std::move(v));
    }
    cmd.argsText = str::join(parts, " ");
    // discord.js の getBoolean("enabled") は型違いで投げる → TS は catch して undefined。
    cmd.enabledOption = s.get_boolean("enabled");
    cmd.userId = s.userId;
    cmd.guildId = s.guildId.value_or("dm");
    cmd.channelId = s.channelId.value_or("default");
    return cmd;
}

Json slash_reply_body(const SlashReply& reply) {
    Json data = Json::object();
    data["content"] = reply.content;
    if (reply.ephemeral) data["flags"] = kEphemeralFlag;
    data["allowed_mentions"] = allowed_mentions_none();
    Json body = Json::object();
    body["type"] = interaction_response::ChannelMessageWithSource;
    body["data"] = data;
    return body;
}

void RestSlashReplier::reply(const SlashReply& reply) { api_.create_interaction_response(id_, token_, slash_reply_body(reply)); }

bool handle_slash_interaction(const Json& interaction, const CommandRouterDeps& deps, SlashReplier& replier) {
    InboundSlashCommand cmd;
    try {
        cmd = inbound_slash_command_from_json(interaction);
    } catch (const std::exception&) {
        log::warn("  discord-gateway: interaction ignored (malformed)");
        return false;
    }
    SlashReply reply;
    bool failed = false;
    try {
        reply = route_slash_command(cmd, deps);
    } catch (const std::exception&) {
        // 例外文は DB のパス等を含みうるので返信にもログにも出さない。
        log::warn("  discord-gateway: interaction failed command=" + cmd.name);
        failed = true;
    }
    if (failed) reply = {"⚠️ internal error", true};
    try {
        replier.reply(reply);
    } catch (const std::exception&) {
        log::warn("  discord-gateway: interaction reply failed command=" + cmd.name);
    }
    return true;
}

InboundRouteResult handle_inbound_message_json(const Json& payload, const std::string& guild_id, const CommandRouterDeps& deps,
                                               const std::optional<std::string>& parent_channel_id) {
    const auto normalized = normalize_discord_inbound_message(payload);
    if (!normalized) return {};
    try {
        return route_inbound_message(*normalized, guild_id, deps, parent_channel_id);
    } catch (const std::exception& e) {
        log::warn(std::string("  discord-gateway: message route failed: ") + e.what());
        return {};
    }
}

void resolve_seed_in_background(const SeedTask& seed, std::function<void(bool started)> on_resolved,
                                const std::function<bool(std::function<void()>)>& submit) {
    if (!seed) return;
    auto task = [seed, on_resolved] {
        const bool started = seed();
        if (on_resolved) {
            try {
                on_resolved(started);
            } catch (const std::exception& e) {
                log::warn(std::string("  discord-seed: callback failed: ") + e.what());
            }
        }
    };
    const bool queued = submit ? submit(task) : background_executor().submit(task);
    if (!queued) {
        log::warn("  discord-seed: background queue full — 議論の種を立てずに捨てる");
        if (on_resolved) on_resolved(false);
    }
}

} // namespace di::discord_hook
