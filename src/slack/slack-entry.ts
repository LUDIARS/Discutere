/**
 * Slack 議論入口の純粋ロジック (テスト対象)。
 *
 * Block Kit の組み立て (議論タイプ select / 進行量 select)、進行量プリセットの解析、
 * Slack message event が「スレッド開始 (トップレベル投稿)」かの判定など、I/O を伴わない
 * 判断を集約する。Slack の event/payload は形が緩いので unknown ベースで安全に取り出す。
 *
 * Discord 版 (forum-flow-tags.ts) と挙動を揃える:
 *   - 議論タイプ value は parseFlowKind が解決できる日本語ラベル ("議論"/"改善"/"学習"/"壁打ち")。
 *   - 進行量プリセットは "default"/"NxM" を rounds/turnsPerRound に解決。
 */

import type { SlackBlock } from "./web-api.js";

/** 議論タイプ select の action_id。 */
export const SLACK_FLOW_PICK_ACTION = "slack-flow-pick";
/** 進行量 select の action_id。 */
export const SLACK_FLOW_SETTINGS_ACTION = "slack-flow-settings";

/** 議論タイプ (value = parseFlowKind が解決できる日本語ラベル)。 */
const FLOW_KIND_OPTIONS: Array<{ value: string; label: string }> = [
  { value: "議論", label: "議論 (中立投票で世論をまとめる)" },
  { value: "改善", label: "改善 (課題抽出 + 改善案)" },
  { value: "学習", label: "学習 (外部の声を収集して KG に取り込む)" },
  { value: "壁打ち", label: "壁打ち (あなたの発言に AI が応答し続ける)" },
];

/** 進行量プリセット (value = "default" or "RxT")。 */
const FLOW_SETTINGS_OPTIONS: Array<{ value: string; label: string }> = [
  { value: "default", label: "標準で開始 (設定ファイルの既定)" },
  { value: "2x4", label: "さくっと (2R×4T)" },
  { value: "3x6", label: "標準量 (3R×6T)" },
  { value: "5x8", label: "じっくり (5R×8T)" },
  { value: "8x12", label: "とことん (8R×12T)" },
];

/** plain_text option を組み立てる小ヘルパ。 */
function plainOption(value: string, label: string): SlackBlock {
  return { text: { type: "plain_text", text: label }, value };
}

/**
 * 議論タイプ選択の Block Kit (案内 section + static_select の actions ブロック)。
 * スレッド開始時に投稿する。選択値は FlowKind 日本語ラベル。
 */
export function buildFlowPickBlocks(): SlackBlock[] {
  return [
    {
      type: "section",
      text: { type: "mrkdwn", text: "*議論タイプを選択してください* (必須)" },
    },
    {
      type: "actions",
      elements: [
        {
          type: "static_select",
          action_id: SLACK_FLOW_PICK_ACTION,
          placeholder: { type: "plain_text", text: "議論タイプを選択" },
          options: FLOW_KIND_OPTIONS.map((o) => plainOption(o.value, o.label)),
        },
      ],
    },
  ];
}

/**
 * 進行量プリセット選択の Block Kit (discussion/improvement の起動前に投稿)。
 * 選択値は parseSettingsPreset が解釈する "default"/"RxT"。
 */
export function buildFlowSettingsBlocks(): SlackBlock[] {
  return [
    {
      type: "section",
      text: { type: "mrkdwn", text: "*進行量を選んで開始* (ラウンド数 × ターン数)" },
    },
    {
      type: "actions",
      elements: [
        {
          type: "static_select",
          action_id: SLACK_FLOW_SETTINGS_ACTION,
          placeholder: { type: "plain_text", text: "進行量を選択" },
          options: FLOW_SETTINGS_OPTIONS.map((o) => plainOption(o.value, o.label)),
        },
      ],
    },
  ];
}

/**
 * プリセット値 ("3x6" / "default") を rounds/turns に解決する。
 * "default" / 不正は {} (= config 既定)。Discord 版 parseSettingsPreset と同仕様。
 */
export function parseSettingsPreset(value: string): { rounds?: number; turnsPerRound?: number } {
  const m = value.match(/^(\d+)x(\d+)$/);
  if (!m) return {};
  return { rounds: Number(m[1]), turnsPerRound: Number(m[2]) };
}

/** Slack の値から文字列を安全に取り出す。 */
function str(obj: Record<string, unknown> | undefined | null, key: string): string | undefined {
  if (!obj) return undefined;
  const v = obj[key];
  return typeof v === "string" ? v : undefined;
}

/**
 * Slack message event が「スレッド開始 (トップレベル投稿)」か。
 *   - thread_ts が無い or thread_ts === ts (= スレッド親)、かつ
 *   - subtype 無し (編集/参加通知等を除外)、かつ
 *   - bot_id 無し (bot 自身の投稿を除外)。
 */
export function isThreadStarter(event: Record<string, unknown> | null | undefined): boolean {
  if (!event) return false;
  if (str(event, "type") !== "message") return false;
  if (str(event, "subtype")) return false;
  if ("bot_id" in event && event.bot_id) return false;
  const ts = str(event, "ts");
  if (!ts) return false;
  const threadTs = str(event, "thread_ts");
  return !threadTs || threadTs === ts;
}

/**
 * block_actions payload から最初の選択値 (static_select の value) を取り出す。
 * 取れなければ undefined。
 */
export function firstSelectedValue(payload: Record<string, unknown>): string | undefined {
  const actions = payload.actions;
  if (!Array.isArray(actions) || actions.length === 0) return undefined;
  const first = actions[0] as Record<string, unknown>;
  const selected = first.selected_option as Record<string, unknown> | undefined;
  return str(selected, "value");
}

/** block_actions payload の最初の action_id を取り出す。 */
export function firstActionId(payload: Record<string, unknown>): string | undefined {
  const actions = payload.actions;
  if (!Array.isArray(actions) || actions.length === 0) return undefined;
  return str(actions[0] as Record<string, unknown>, "action_id");
}
