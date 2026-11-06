// DiscordGateway::Impl のメンバ関数: MessageReactionAdd・InteractionCreate。宣言と状態は gateway_impl.hpp。
#include "discord_hook/gateway_impl.hpp"

namespace di::discord_hook {

auto DiscordGateway::Impl::handle_reaction(const ForumReactionEvent& ev) -> void {
    try {
        if (ev.user_bot) return;
        const Json& d = ev.raw;
        const auto& emoji_obj = json_object(d, "emoji");
        std::string emoji = json_string(emoji_obj, "name");
        if (emoji.empty()) emoji = json_string(emoji_obj, "id");
        const std::string message_id = json_string(d, "message_id");
        const std::string channel_id = json_string(d, "channel_id");
        if (message_id.empty() || emoji.empty()) return;
        // ペーパーレビュー承認: ✅ をレビュー中スレッドに付けたら議論を開始する (スコアリングより優先)。
        if (emoji == "✅" && flow_live && has_review(channel_id)) {
            auto self = shared_from_this();
            const std::string guild = json_opt_string(d, "guild_id").value_or("dm");
            const bool queued = submit_background([self, channel_id, guild] {
                try {
                    flow::handle_paper_review_approval(channel_id, *self->flow_live, self->flow_hooks, guild);
                } catch (const std::exception& e) {
                    log::warn("  discord-forum: ペーパー承認 (reaction) 失敗: " + what_of(e));
                }
            });
            if (!queued) log::error("  discord-forum: 背景キューが満杯のため承認を処理できない (thread=" + channel_id + ")");
            return;
        }
        if (deps.onReaction) deps.onReaction(ReactionInfo{message_id, channel_id, emoji, json_string(d, "user_id")});
    } catch (const std::exception& e) {
        log::warn("  discord-gateway: reaction handle failed: " + what_of(e));
    }
}

auto DiscordGateway::Impl::handle_interaction(const ForumInteractionEvent& ev) -> void {
    const Json& d = ev.raw;
    Ix ix(api, d);
    if (!ix.valid()) return;
    const auto type = json_int(d, "type", 0);
    const auto& data = json_object(d, "data");
    const std::string custom_id = json_string(data, "custom_id");
    const std::string channel_id = json_string(d, "channel_id");

    if (type == 3) {
        const auto component_type = json_int(data, "component_type", 0);
        // ボタン: 進行量「数値を指定」(flow-custom:<threadId>) → モーダル表示 / 続行・停止ボタン。
        if (component_type == 2) {
            if (const auto thread_id = parse_flow_custom_btn_custom_id(custom_id)) {
                bool has = false;
                {
                    std::lock_guard lock(mu);
                    has = pending_settings.has(*thread_id);
                }
                if (has) ix.quietly([&] { ix.show_modal(build_flow_settings_modal(*thread_id)); });
                else ix.quietly([&] { ix.reply(kSettingsExpired, true); });
                return;
            }
            if (debate_runner) {
                try {
                    discussion::ButtonInteraction b{json_string(d, "id"), json_string(d, "token"), custom_id, json_opt_string(d, "channel_id")};
                    debate_runner->handle_button(b);
                } catch (...) {
                }
            }
            return;
        }
        if (component_type == 3) handle_select(ix, data, custom_id);
        return;
    }
    if (type == 5) {
        handle_modal(ix, d, data, custom_id, channel_id);
        return;
    }
    if (type != 2) return;
    handle_command(ix, d, data, channel_id);
}

auto DiscordGateway::Impl::handle_select(Ix& ix, const Json& data, const std::string& custom_id) -> void {
    const auto& values = json_array(data, "values");
    const std::string value = !values.empty() && values[0].is_string() ? values[0].get<std::string>() : std::string();
    // 進行量プリセット (flow-settings:<threadId>) → rounds/turns を決めて起動 (item1)。
    if (const auto settings_thread = parse_flow_settings_custom_id(custom_id)) {
        const auto preset = parse_settings_preset(value);
        const bool started = start_from_settings(*settings_thread, preset.rounds, preset.turnsPerRound);
        ix.quietly([&] { ix.update(started ? "✅ 進行量「" + value + "」で開始します" : std::string(kSettingsExpired)); });
        return;
    }
    // 議論タイプ select (flow-pick:<threadId>) — タグ無し投稿のフロー選択。
    const auto thread_id = parse_flow_pick_custom_id(custom_id);
    if (!thread_id) return;
    std::optional<PendingFlowPick> pending;
    {
        std::lock_guard lock(mu);
        pending = pending_picks.get(*thread_id);
    }
    const auto flow = flow::parse_flow_kind(std::optional<std::string_view>(value));
    if (!pending || !flow || !flow_live) {
        ix.quietly([&] { ix.reply(kPickExpired, true); });
        return;
    }
    {
        std::lock_guard lock(mu);
        pending_picks.take(*thread_id);
    }
    // フロー再提案からの選び直し (09): 議論/改善なら今のペーパーのまま続ける。
    bool continued = false;
    try {
        continued = flow::continue_paper_review_with_flow(*thread_id, *flow, *flow_live, pending->guildId);
    } catch (const std::exception& e) {
        log::warn("  discord-forum: ペーパー続行失敗 (thread=" + *thread_id + "): " + what_of(e));
    }
    if (continued) {
        ix.quietly([&] { ix.update("✅ 「" + value + "」を選択 (今のペーパーで続行)"); });
        return;
    }
    // 壁打ち/学習を選んだら、進行中のペーパーレビュー待ちは破棄して新タイプで起動する。
    try {
        flow::cancel_paper_review(*thread_id);
    } catch (...) {
    }
    ix.quietly([&] { ix.update("✅ 「" + value + "」を選択"); });
    begin_flow(BeginFlowInfo{pending->guildId, *thread_id, pending->theme, *flow, pending->tags, pending->rounds, pending->turnsPerRound,
                             pending->opponentPersonaIds, pending->specText, pending->specAttachmentUrls});
}

auto DiscordGateway::Impl::handle_modal(Ix& ix, const Json& d, const Json& data, const std::string& custom_id, const std::string& channel_id) -> void {
    (void)channel_id;
    const auto fields = modal_field_values(data);
    auto field = [&](const char* id) {
        auto it = fields.find(id);
        return it == fields.end() ? std::string() : str::trim(it->second);
    };
    if (const auto gap = parse_paper_gap_modal_custom_id(custom_id)) {
        const std::string detail = field(paper_gap_field_ids::detail);
        if (detail.empty()) {
            ix.quietly([&] { ix.reply("不足事項の内容を入力してください。", true); });
            return;
        }
        if (!flow_live) {
            ix.quietly([&] { ix.reply("ペーパー調整機能が無効です (flowLive 未設定)。", true); });
            return;
        }
        if (!has_review(gap->channelId)) {
            ix.quietly([&] { ix.reply(kNoPaperForGap, true); });
            return;
        }
        PaperGapInstructionInput in;
        in.type = gap->type;
        const auto target = field(paper_gap_field_ids::target);
        const auto source = field(paper_gap_field_ids::source);
        in.target = target;
        in.detail = detail;
        in.source = source;
        const std::string instruction = format_paper_gap_review_instruction(in);
        try {
            ix.defer_ephemeral();
        } catch (...) {
            log::warn("  discord-paper-gap: modal failed id=" + json_string(d, "id"));
            return;
        }
        // ペーパーの調整は LLM を待つ: defer 済みなので背景で処理して editReply で結果を返す。
        auto self = shared_from_this();
        auto shared_ix = std::make_shared<Ix>(ix);
        const std::string guild = json_opt_string(d, "guild_id").value_or("dm");
        const std::string gap_channel = gap->channelId;
        const std::string interaction_id = json_string(d, "id");
        const bool queued = submit_background([self, shared_ix, guild, gap_channel, instruction, interaction_id] {
            try {
                const bool handled = flow::handle_paper_review_reply(gap_channel, guild, instruction, *self->flow_live, self->flow_hooks);
                shared_ix->edit_reply(handled ? "不足事項を受け付けました。ペーパーの更新結果をスレッドに投稿します。"
                                              : "不足事項は受け付けましたが、調整待ちのペーパーに接続できませんでした。");
            } catch (const std::exception& e) {
                log::warn("  discord-paper-gap: modal failed id=" + interaction_id + ": " + what_of(e));
                shared_ix->quietly([&] { shared_ix->edit_reply("不足事項の反映に失敗しました。"); });
            }
        });
        if (!queued) ix.quietly([&] { ix.edit_reply("不足事項の反映に失敗しました。"); });
        return;
    }
    // 進行量モーダル送信 (flow-modal:<threadId>) → 任意の rounds/turns で起動 (item1)。
    const auto modal_thread = parse_flow_modal_custom_id(custom_id);
    if (!modal_thread) return;
    const auto rounds = positive_number_field(field("rounds"));
    const auto turns = positive_number_field(field("turns"));
    const bool started = start_from_settings(*modal_thread, rounds, turns);
    const std::string content =
        started ? "✅ 進行量 (ラウンド " + (rounds ? js_number_to_string(*rounds) : std::string("既定")) + " × ターン " +
                      (turns ? js_number_to_string(*turns) : std::string("既定")) + ") で開始します"
                : std::string(kSettingsExpired);
    ix.quietly([&] { ix.reply(content, !started); });
}

auto DiscordGateway::Impl::handle_command(Ix& ix, const Json& d, const Json& data, const std::string& channel_id) -> void {
    const std::string name = json_string(data, "name");
    if (name == MECHANICS_CHECK_COMMAND_NAME || name == LEVEL_CHECK_COMMAND_NAME) {
        try {
            const SlashInteraction si = slash_interaction_from_json(d);
            // 3 秒の応答期限を守るため先に defer し、LLM の診断は背景で行う。
            RestInteractionResponder(*api, si).defer_ephemeral();
            auto self = shared_from_this();
            const bool queued = submit_background([self, si, name] {
                try {
                    PreDeferredResponder responder(RestInteractionResponder(*self->api, si));
                    if (name == MECHANICS_CHECK_COMMAND_NAME) handle_mechanics_check_command(si, responder, self->deps.mechanicsCheck.get());
                    else handle_level_check_command(si, responder, self->deps.levelCheck.get());
                } catch (...) {
                    // 利用者の入力やバックエンドの失敗をログに出さず、汎用ルータにも落とさない。
                    log::warn("  discord-gateway: diagnostic reply delivery failed");
                }
            });
            if (!queued) {
                try {
                    RestInteractionResponder(*api, si).edit_reply(DiagnosticReply{diagnostic_failure(nullptr), std::nullopt});
                } catch (...) {
                    log::warn("  discord-gateway: diagnostic reply delivery failed");
                }
            }
        } catch (...) {
            log::warn("  discord-gateway: diagnostic reply delivery failed");
        }
        return;
    }

    if (name == PAPER_GAP_COMMAND_NAME) {
        std::string raw_type;
        for (const auto& o : json_array(data, "options"))
            if (o.is_object() && json_string(o, "name") == "type" && o.contains("value") && o["value"].is_string())
                raw_type = o["value"].get<std::string>();
        if (!is_paper_gap_type(raw_type)) {
            ix.quietly([&] { ix.reply("不足事項の種類が不正です。", true); });
            return;
        }
        if (!flow_live) {
            ix.quietly([&] { ix.reply("ペーパー調整機能が無効です (flowLive 未設定)。", true); });
            return;
        }
        if (!has_review(channel_id)) {
            ix.quietly([&] { ix.reply(kNoPaperForGap, true); });
            return;
        }
        ix.quietly([&] { ix.show_modal(build_paper_gap_modal(raw_type, channel_id)); });
        return;
    }

    // /debate: パーティ議論を開始 (非同期、ack だけ即返す)。
    if (name == "debate") {
        if (!debate_runner) {
            ix.quietly([&] { ix.reply("議論機能が無効です (config.discussion 未設定)", true); });
            return;
        }
        std::string topic;
        for (const auto& o : json_array(data, "options"))
            if (o.is_object() && json_string(o, "name") == "topic" && o.contains("value") && o["value"].is_string())
                topic = o["value"].get<std::string>();
        ix.quietly([&] { ix.reply("🗣️ 議論を開始します: 「" + topic + "」", true); });
        auto self = shared_from_this();
        if (!submit_background([self, channel_id, topic] {
                try {
                    if (self->debate_runner) self->debate_runner->start(channel_id, topic);
                } catch (const std::exception& e) {
                    log::warn("  discord-gateway: debate 失敗: " + what_of(e));
                }
            }))
            log::warn("  discord-gateway: 背景キューが満杯のため debate を開始できない");
        return;
    }

    RestSlashReplier replier(*api, json_string(d, "id"), json_string(d, "token"));
    try {
        handle_slash_interaction(d, deps.router, replier);
    } catch (...) {
        log::warn("  discord-gateway: interaction failed id=" + json_string(d, "id"));
    }
}

} // namespace di::discord_hook
