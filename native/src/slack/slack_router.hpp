#pragma once
// src/slack/slack-router.ts の移植: Slack Socket Mode のルーティング。
//
// - on_event: 設定チャンネルでのスレッド開始投稿に議論タイプ選択 (block) を出す。
//   壁打ち継続中スレッドへの返信は handle_slack_flow_reply に流す。bot 自身の投稿は無視。
// - on_interactive: block_actions の選択を見て、議論タイプ → (進行量 or 即起動)、
//   進行量 → start_slack_flow に繋ぐ。pending 状態は thread_ts キーの表で持つ
//   (Discord の pendingFlowPicks/pendingFlowSettings 相当、mutex で守る)。
//
// Slack の user 名/ID は保存しない (個人データ方針)。テーマは投稿テキストのみ。
// 設定チャンネル (channelIds) 以外のイベントは無視する (TS と同じ照合)。
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "base/json.hpp"
#include "slack/slack_live.hpp"

namespace di::slack {

/** pending 表 (選択待ち) の上限。超えたら古いものから捨てる (移植で追加。TS は無制限)。 */
inline constexpr std::size_t kMaxPendingThreads = 1000;

struct SlackRouterConfig {
    /** 議論を起こす Slack channel id。これ以外のチャンネルは無視。 */
    std::vector<std::string> channelIds;
    SlackFlowDeps deps;
    SlackFlowHooks hooks;
    /**
     * 議論の起動 (TS の void startSlackFlow) を積む先。既定は background_executor().submit。
     * false (満杯) なら起動しない (warn)。
     */
    std::function<bool(std::function<void()>)> submit;
};

struct SlackRouter {
    std::function<void(const Json& event)> onEvent;
    std::function<void(const Json& payload)> onInteractive;
};

/** Slack ルータを生成する。start_socket_mode の onEvent/onInteractive に結線する。 */
SlackRouter create_slack_router(SlackRouterConfig cfg);

} // namespace di::slack
