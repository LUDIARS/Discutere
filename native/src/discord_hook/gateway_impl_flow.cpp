// DiscordGateway::Impl のメンバ関数: ThreadCreate (フロー起動)・MessageCreate。宣言と状態は gateway_impl.hpp。
#include "discord_hook/gateway_impl.hpp"

namespace di::discord_hook {

/** starter message を取得 (作成直後は未確定なことがあるので 1 回リトライ)。 */
auto DiscordGateway::Impl::fetch_starter_with_retry(const std::string& thread_id) -> std::optional<Message> {
    for (int attempt = 0; attempt < 2; ++attempt) {
        try {
            return api->get_message(thread_id, thread_id);
        } catch (...) {
            // 作成直後は未確定 → リトライ
        }
        sleep_for(std::chrono::milliseconds(500));
    }
    return std::nullopt;
}

/** フォーラム starter からテーマ + 適用タグ名 + guildId を解決する。管理forum ID境界の外 / starter が bot なら nullopt。 */
auto DiscordGateway::Impl::resolve_forum_starter(const Json& d) -> std::optional<ForumStarter> {
    Channel thread;
    try {
        thread = channel_from_json(d);
    } catch (...) {
        return std::nullopt;
    }
    if (!managed_location(thread.guild_id, thread.id, forum_ref_from_channel(thread))) return std::nullopt;
    if (!thread.parent_id) return std::nullopt;
    std::optional<Channel> parent;
    try {
        parent = api->get_channel(*thread.parent_id);
    } catch (...) {
        return std::nullopt;  // 取得不可 → 判定不能
    }
    if (parent->type != channel_type::GuildForum) return std::nullopt;
    if (!thread.guild_id || thread.guild_id->empty()) return std::nullopt;
    ForumStarter s;
    s.guildId = *thread.guild_id;
    s.appliedTagNames = thread_applied_tag_names(thread, parent);
    const auto starter = fetch_starter_with_retry(thread.id);
    if (!starter) {
        log::warn("  discord-forum: starter message 取得不可 (thread=" + thread.id + ")");
        return std::nullopt;
    }
    if (starter->author.bot) return std::nullopt;
    // 議題はスレッド名を優先 (本文にゲーム名が無くても議題を固定する)。
    s.starterContent = str::trim(starter->content);
    const std::string name = thread.name ? str::trim(*thread.name) : std::string();
    s.theme = !name.empty() ? name : s.starterContent;
    if (s.theme.empty()) return std::nullopt;
    // テキスト系の添付ファイル URL を仕様書解析 (③) の材料にする (画像/バイナリは除外)。
    for (const auto& a : starter->attachments)
        if (is_text_like_attachment(a.filename, a.content_type)) s.specAttachmentUrls.push_back(a.url);
    return s;
}

auto DiscordGateway::Impl::post_components(const std::string& thread_id, const std::optional<std::string>& guild_id, const std::string& content, Json components, const char* what) -> void {
    try {
        if (!managed_location(guild_id, thread_id)) return;
        api->create_message(thread_id, Json{{"content", content}, {"components", std::move(components)}});
    } catch (const std::exception& e) {
        log::warn(std::string("  discord-forum: ") + what + " menu 失敗 (thread=" + thread_id + "): " + what_of(e));
    }
}

/** 議論タイプ select メニューをスレッドに出す。 */
auto DiscordGateway::Impl::post_flow_pick_menu(const std::string& thread_id, const std::string& prefix) -> void {
    std::optional<std::string> guild;
    {
        std::lock_guard lock(mu);
        if (auto p = pending_picks.get(thread_id)) guild = p->guildId;
    }
    post_components(thread_id, guild, prefix + kFlowPickPrompt, Json::array({build_flow_pick_menu(thread_id)}), "flow-pick");
}

/** 進行量設定 UI (プリセット select + 数値指定ボタン) をスレッドに出す。 */
auto DiscordGateway::Impl::post_flow_settings_menu(const std::string& thread_id) -> void {
    std::optional<std::string> guild;
    {
        std::lock_guard lock(mu);
        if (auto p = pending_settings.get(thread_id)) guild = p->guildId;
    }
    Json rows = Json::array();
    for (auto& r : build_flow_settings_components(thread_id)) rows.push_back(std::move(r));
    post_components(thread_id, guild, kFlowSettingsPrompt, std::move(rows), "flow-settings");
}

/** フローの起動を背景キューへ (完走を待たない)。満杯なら選択メニューを出し直して再試行を促す。 */
auto DiscordGateway::Impl::launch_flow(const flow::StartForumFlowInput& input) -> void {
    auto self = shared_from_this();
    const bool queued = submit_background([self, input] {
        if (!self->flow_live) return;
        flow::start_forum_flow(input, *self->flow_live, self->flow_hooks);
    });
    if (queued) return;
    log::error("  discord-forum: 背景キューが満杯のためフローを起動できない (thread=" + input.threadId + ")");
    {
        std::lock_guard lock(mu);
        pending_picks.set(input.threadId, PendingFlowPick{input.guildId, input.theme, input.tags, input.rounds, input.turnsPerRound,
                                                          input.opponentPersonaIds, input.specText, input.specAttachmentUrls});
    }
    post_flow_pick_menu(input.threadId, "⚠️ 混雑のため開始できませんでした。もう一度選んでください。\n");
}

auto DiscordGateway::Impl::to_input(const BeginFlowInfo& info) -> flow::StartForumFlowInput {
    flow::StartForumFlowInput in;
    in.guildId = info.guildId;
    in.threadId = info.threadId;
    in.theme = info.theme;
    in.flow = info.flow;
    in.tags = info.tags;
    in.rounds = info.rounds;
    in.turnsPerRound = info.turnsPerRound;
    in.opponentPersonaIds = info.opponentPersonaIds;
    in.specText = info.specText;
    in.specAttachmentUrls = info.specAttachmentUrls;
    return in;
}

/**
 * フロー起動の入口 (item1)。discussion/improvement で進行量が本文未指定なら進行量設定 UI を出して待つ。
 * それ以外 (learning/sparring、または本文指定済み) は即起動。
 */
auto DiscordGateway::Impl::begin_flow(const BeginFlowInfo& info) -> void {
    if (!flow_live) return;
    const bool wants_settings = (info.flow == "discussion" || info.flow == "improvement") && !info.rounds && !info.turnsPerRound;
    if (wants_settings) {
        {
            std::lock_guard lock(mu);
            pending_settings.set(info.threadId, PendingFlowSettings{info.guildId, info.theme, info.flow, info.tags, info.opponentPersonaIds,
                                                                    info.specText, info.specAttachmentUrls});
        }
        post_flow_settings_menu(info.threadId);
        return;
    }
    launch_flow(to_input(info));
}

/** 進行量設定 (select/modal) を受けて pending を起動する。期限切れなら false。 */
auto DiscordGateway::Impl::start_from_settings(const std::string& thread_id, std::optional<double> rounds, std::optional<double> turns) -> bool {
    std::optional<PendingFlowSettings> pending;
    {
        std::lock_guard lock(mu);
        if (!flow_live || !pending_settings.has(thread_id)) return false;
        pending = pending_settings.take(thread_id);
    }
    BeginFlowInfo info{pending->guildId, thread_id, pending->theme, pending->flow, pending->tags, rounds, turns,
                       pending->opponentPersonaIds, pending->specText, pending->specAttachmentUrls};
    launch_flow(to_input(info));
    return true;
}

/** フォーラム starter を解析し、フロー起動 or タグ選択 UI を出す。 */
auto DiscordGateway::Impl::on_forum_thread_create(const Json& d) -> void {
    if (!flow_live) return;
    const auto starter = resolve_forum_starter(d);
    if (!starter) return;
    const std::string thread_id = json_string(d, "id");
    const auto& p = deps.entryParsers;
    ParsedForumEntry entry = p.parseForumEntry ? p.parseForumEntry(starter->appliedTagNames) : ParsedForumEntry{};
    // 本文/タイトルの「ラウンド:N ターン:M」「相手:名前」記法で議論ごとの指定 (任意)。
    const ParsedRoundsTurns rt = p.parseRoundsTurns ? p.parseRoundsTurns(starter->theme) : ParsedRoundsTurns{};
    const auto opponents = p.parseOpponents ? p.parseOpponents(starter->theme) : std::nullopt;
    // starter 本文がタイトル (議題) と別物なら仕様書解析の材料にする (② 学習のみで使用)。
    std::optional<std::string> spec_text;
    if (!starter->starterContent.empty() && starter->starterContent != starter->theme) spec_text = starter->starterContent;
    std::optional<std::vector<std::string>> attachments;
    if (!starter->specAttachmentUrls.empty()) attachments = starter->specAttachmentUrls;
    if (entry.flow) {
        begin_flow(BeginFlowInfo{starter->guildId, thread_id, starter->theme, *entry.flow, entry.tags, rt.rounds, rt.turnsPerRound,
                                 opponents, spec_text, attachments});
        return;
    }
    // 議論タイプタグ無し → 選択 UI を出して待つ。
    {
        std::lock_guard lock(mu);
        pending_picks.set(thread_id, PendingFlowPick{starter->guildId, starter->theme, entry.tags, rt.rounds, rt.turnsPerRound,
                                                     opponents, spec_text, attachments});
    }
    post_flow_pick_menu(thread_id);
}

/** 既存スレッドを取得し、新規投稿の THREAD_CREATE と同じ門と起動経路へ渡す。 */
auto DiscordGateway::Impl::start_existing_thread(const std::string& thread_id) -> StartExistingThreadResult {
    if (!flow_live) return {false, "flow not available"};
    Json d;
    Channel thread;
    try {
        d = api->request("GET", "/channels/" + thread_id);
        thread = channel_from_json(d);
    } catch (const std::exception&) {
        return {false, "thread not found"};
    }
    if (!managed_location(thread.guild_id, thread.id, forum_ref_from_channel(thread))) return {false, "outside managed forum"};
    {
        std::lock_guard lock(mu);
        if (pending_picks.get(thread_id)) return {false, "already waiting for a flow choice"};
    }
    if (has_review(thread_id)) return {false, "already started"};
    d["newly_created"] = true;
    auto self = shared_from_this();
    enqueue(true, "THREAD_CREATE", [self, d] { self->on_thread_create(d); });
    return {true, ""};
}

auto DiscordGateway::Impl::has_review(const std::string& channel_id) -> bool {
    try {
        return flow::has_paper_review(channel_id);
    } catch (const std::exception& e) {
        log::warn("  discord-forum: レビュー状態の確認失敗: " + what_of(e));
        return false;
    }
}

auto DiscordGateway::Impl::handle_message(const ForumMessageEvent& ev) -> void {
    Message msg;
    try {
        msg = message_from_json(ev.raw);
    } catch (...) {
        return;  // 形の壊れた投稿は捨てる
    }
    if (msg.author.bot) return;
    // Discord のリプライは宛先に関係なく対象外。通常投稿の回答だけを受け付ける。
    if (msg.message_reference) return;

    // ゲーム感想チャンネル: カテゴリ「ゲーム感想」配下の投稿は議論にせず感想収集 (匿名)。
    if (deps.gameFeedback && deps.gameFeedback->enabled) {
        try {
            const Channel channel = api->get_channel(msg.channel_id);
            std::optional<Channel> parent;
            if (channel.parent_id) {
                try {
                    parent = api->get_channel(*channel.parent_id);
                } catch (...) {
                }
            }
            if (const auto fb = extract_game_feedback(msg, channel, parent, deps.gameFeedback->categoryName)) {
                try {
                    if (deps.onGameFeedback) deps.onGameFeedback(GameFeedbackInput{fb->gameTitle, fb->content, fb->createdAt});
                    try {
                        api->create_reaction(msg.channel_id, msg.id, "📝");
                    } catch (...) {
                    }
                } catch (const std::exception& e) {
                    log::warn("  game-feedback: 収集失敗: " + what_of(e));
                }
                return;  // 感想は議論ルーティングに回さない
            }
        } catch (const std::exception& e) {
            log::warn("  game-feedback: チャンネル取得失敗: " + what_of(e));
        }
    }

    const bool starter = is_forum_starter_message(msg.id, msg.channel_id);
    // レビュー中のスレッドは調整/承認として扱う (メンションがあっても調整指示に回さない)。
    if (forum_enabled && flow_live && !starter && has_review(msg.channel_id)) {
        route_discussion_message(msg, ev.raw, starter);
        return;
    }
    if (!maybe_handle_directive(msg)) route_discussion_message(msg, ev.raw, starter);
}

/**
 * bot (@Discutere) へのメンションを「進行役への調整指示」として取り込む (門を通った管理フォーラムでのみ)。
 * @returns true なら調整指示として処理済 (通常ルーティングを行わない)。
 */
auto DiscordGateway::Impl::maybe_handle_directive(const Message& msg) -> bool {
    std::string bot_id;
    {
        std::lock_guard lock(mu);
        bot_id = self_id;
    }
    if (bot_id.empty()) return false;
    bool mentioned = false;
    for (const auto& u : msg.mentions) mentioned = mentioned || u.id == bot_id;
    if (!mentioned) return false;
    const std::string text = strip_bot_mention(msg.content, bot_id);
    try {
        const auto result = handle_directive_message(DirectiveMessageInput{deps.router.workspaceId, msg.guild_id.value_or("dm"), msg.channel_id,
                                                                           text, msg.author.id.empty() ? std::nullopt : std::optional(msg.author.id)});
        try {
            post_discord_channel(*api, DiscordPostArgs{msg.channel_id, result.reply, msg.id});
        } catch (...) {
        }
    } catch (const std::exception& e) {
        log::warn("  discord-directive: failed: " + what_of(e));
    }
    return true;
}

/** 共通gate後の管理フォーラム内ルーティング。 */
auto DiscordGateway::Impl::route_discussion_message(const Message& msg, const Json& raw, bool starter) -> void {
    // フォーラムスレッド内の後続投稿: (1) ペーパーレビュー中なら調整/承認、(2) 中断した議論のやり直し、(3) 壁打ちの相手発話。
    if (forum_enabled) {
        if (!starter && flow_live) {
            auto self = shared_from_this();
            const Message m = msg;
            const bool queued = submit_background([self, m] {
                try {
                    const auto& fl = *self->flow_live;
                    const std::string guild = m.guild_id.value_or("dm");
                    // 受付チェックは編集の有無と別。準備中の回答も対象にし、LLM の処理完了を待たずに受付を知らせる。
                    ack_paper_review_reply(ackable_message(*self->api, m), self->has_review(m.channel_id));
                    if (flow::handle_paper_review_reply(m.channel_id, guild, m.content, fl, self->flow_hooks)) return;
                    // 再起動などで途中で止まった議論は「再開」返信で同じペーパーからやり直す。
                    if (flow::handle_interrupted_discussion_reply(m.channel_id, guild, m.content, fl, self->flow_hooks)) {
                        ack_paper_review_reply(ackable_message(*self->api, m), true);
                        return;
                    }
                    flow::handle_forum_flow_reply(m.channel_id, guild, m.content, fl, self->flow_hooks);
                } catch (const std::exception& e) {
                    log::warn("  discord-forum: flow reply 失敗: " + what_of(e));
                }
            });
            if (!queued) log::warn("  discord-forum: 背景キューが満杯のため返信を処理できない (thread=" + msg.channel_id + ")");
        }
        return;
    }

    // データクロール用チャンネル: 貼られた URL を外部データ取り込みに回す (議論取り込みはしない)。
    std::optional<std::string> parent_id;
    try {
        const auto ref = fetch_channel(msg.channel_id);
        if (ref && ref->is_thread) parent_id = ref->parent_id;
    } catch (...) {
    }
    bool crawl_target = false;
    {
        std::lock_guard lock(mu);
        crawl_target = crawl_ids.count(msg.channel_id) || (parent_id && crawl_ids.count(*parent_id));
    }
    if (deps.crawlDeps && crawl_target) {
        auto self = shared_from_this();
        const Message m = msg;
        const bool queued = submit_background([self, m] {
            try {
                handle_crawl_message(*self->api, m, CrawlHandlerDeps{*self->deps.crawlDeps, std::nullopt, std::nullopt});
            } catch (const std::exception& e) {
                log::warn("  discord-gateway: crawl 失敗: " + what_of(e));
            }
        });
        if (!queued) log::warn("  discord-gateway: 背景キューが満杯のため crawl を見送る");
        return;
    }

    const auto normalized = normalize_discord_inbound_message(raw);
    if (!normalized) return;
    try {
        const auto r = route_inbound_message(*normalized, msg.guild_id.value_or("dm"), deps.router, parent_id);
        // 取り込んだ全件ではなく「議論の種になった投稿」にだけ 👀 を付ける。
        if (r.ingested && r.seed) {
            auto api_ref = api;
            const std::string channel = msg.channel_id, id = msg.id;
            resolve_seed_in_background(
                r.seed,
                [api_ref, channel, id](bool started) {
                    if (!started) return;
                    try {
                        api_ref->create_reaction(channel, id, "👀");
                    } catch (...) {
                    }
                },
                deps.router.submitBackground);
        }
    } catch (const std::exception& e) {
        log::warn("  discord-gateway: message route failed: " + what_of(e));
    }
}

} // namespace di::discord_hook
