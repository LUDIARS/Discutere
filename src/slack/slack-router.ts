/**
 * Slack Socket Mode のルーティング。
 *
 * - onEvent: 設定チャンネルでのスレッド開始投稿に議論タイプ選択 (block) を出す。
 *   壁打ち継続中スレッドへの返信は handleSlackFlowReply に流す。bot 自身の投稿は無視。
 * - onInteractive: block_actions の選択を見て、議論タイプ → (進行量 or 即起動)、
 *   進行量 → startSlackFlow に繋ぐ。pending 状態は thread_ts キーの Map で持つ
 *   (Discord の pendingFlowPicks/pendingFlowSettings 相当)。
 *
 * Slack の user 名/ID は保存しない (個人データ方針)。テーマは投稿テキストのみ。
 */

import { parseFlowKind, type FlowKind } from "../flow/dispatch.js";
import { postMessage } from "./web-api.js";
import {
  SLACK_FLOW_PICK_ACTION,
  SLACK_FLOW_SETTINGS_ACTION,
  buildFlowPickBlocks,
  buildFlowSettingsBlocks,
  firstActionId,
  firstSelectedValue,
  isThreadStarter,
  parseSettingsPreset,
} from "./slack-entry.js";
import {
  handleSlackFlowReply,
  hasSlackSparringSession,
  startSlackFlow,
  type SlackFlowDeps,
  type SlackFlowHooks,
} from "./slack-live.js";

export interface SlackRouterConfig {
  /** 議論を起こす Slack channel id。これ以外のチャンネルは無視。 */
  channelIds: string[];
  deps: SlackFlowDeps;
  hooks?: SlackFlowHooks;
}

/** flow-pick 後・進行量選択待ちの保留 (thread_ts → {channel, theme, flow})。 */
interface PendingFlow {
  channel: string;
  theme: string;
  flow: FlowKind;
}

/**
 * Slack ルータを生成する。startSocketMode の onEvent/onInteractive に結線する。
 */
export function createSlackRouter(cfg: SlackRouterConfig): {
  onEvent: (event: Record<string, unknown>) => Promise<void>;
  onInteractive: (payload: Record<string, unknown>) => Promise<void>;
} {
  const channelSet = new Set(cfg.channelIds);
  // flow-pick 待ち (議論タイプ未選択): thread_ts → theme。
  const pendingPicks = new Map<string, { channel: string; theme: string }>();
  // 進行量待ち (discussion/improvement): thread_ts → PendingFlow。
  const pendingSettings = new Map<string, PendingFlow>();

  /** message event から文字列を安全に取り出す。 */
  function str(obj: Record<string, unknown>, key: string): string | undefined {
    const v = obj[key];
    return typeof v === "string" ? v : undefined;
  }

  async function onEvent(event: Record<string, unknown>): Promise<void> {
    if (str(event, "type") !== "message") return;
    if ("bot_id" in event && event.bot_id) return; // bot 自身の投稿は無視
    const channel = str(event, "channel");
    if (!channel || !channelSet.has(channel)) return; // 設定チャンネル外は無視

    const ts = str(event, "ts");
    const threadTs = str(event, "thread_ts");
    const text = str(event, "text") ?? "";

    // ── 壁打ち継続中スレッドへの返信なら submitUser に流す ──
    if (threadTs && hasSlackSparringSession(threadTs)) {
      await handleSlackFlowReply(threadTs, channel, text, cfg.deps, cfg.hooks);
      return;
    }

    // ── スレッド開始 (トップレベル投稿) なら議論タイプ選択を出す ──
    if (isThreadStarter(event) && ts) {
      pendingPicks.set(ts, { channel, theme: text });
      try {
        await postMessage(cfg.deps.botToken, {
          channel,
          threadTs: ts, // スレッド (= ts) に選択 UI を出す
          text: "議論タイプを選択してください",
          blocks: buildFlowPickBlocks(),
        });
      } catch (err) {
        console.warn(`  slack-router: flow-pick post 失敗: ${(err as Error).message}`);
      }
    }
  }

  /** interactive payload から channel id / thread_ts を安全に取り出す。 */
  function resolveThread(payload: Record<string, unknown>): { channel?: string; threadTs?: string } {
    const channelObj = payload.channel as Record<string, unknown> | undefined;
    const channel = typeof channelObj?.id === "string" ? channelObj.id : undefined;
    const message = payload.message as Record<string, unknown> | undefined;
    const threadTs =
      (typeof message?.thread_ts === "string" ? message.thread_ts : undefined) ??
      (typeof message?.ts === "string" ? message.ts : undefined);
    return { channel, threadTs };
  }

  async function onInteractive(payload: Record<string, unknown>): Promise<void> {
    if (str(payload, "type") !== "block_actions") return;
    const actionId = firstActionId(payload);
    const value = firstSelectedValue(payload);
    if (!actionId || !value) return;
    const { channel, threadTs } = resolveThread(payload);
    if (!channel || !threadTs) return;

    // ── 議論タイプ選択 ──
    if (actionId === SLACK_FLOW_PICK_ACTION) {
      const flow = parseFlowKind(value);
      if (!flow) return;
      const pending = pendingPicks.get(threadTs);
      const theme = pending?.theme ?? "";
      pendingPicks.delete(threadTs);

      if (flow === "discussion" || flow === "improvement") {
        // 進行量選択待ちに移す。
        pendingSettings.set(threadTs, { channel, theme, flow });
        try {
          await postMessage(cfg.deps.botToken, {
            channel,
            threadTs,
            text: "進行量を選んで開始してください",
            blocks: buildFlowSettingsBlocks(),
          });
        } catch (err) {
          console.warn(`  slack-router: flow-settings post 失敗: ${(err as Error).message}`);
        }
      } else {
        // learning / sparring は即起動。
        void startSlackFlow({ channel, threadTs, theme, flow, tags: [] }, cfg.deps, cfg.hooks);
      }
      return;
    }

    // ── 進行量選択 → discussion/improvement を起動 ──
    if (actionId === SLACK_FLOW_SETTINGS_ACTION) {
      const pending = pendingSettings.get(threadTs);
      if (!pending) return;
      pendingSettings.delete(threadTs);
      const { rounds, turnsPerRound } = parseSettingsPreset(value);
      void startSlackFlow(
        { channel: pending.channel, threadTs, theme: pending.theme, flow: pending.flow, tags: [], rounds, turnsPerRound },
        cfg.deps,
        cfg.hooks
      );
      return;
    }
  }

  return { onEvent, onInteractive };
}
