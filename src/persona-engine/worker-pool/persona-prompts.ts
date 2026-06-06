/**
 * 常駐ワーカーの standing persona prompt 生成。
 *
 * この本文は spawn 時に Lictor の delegation auto-inject 機構で「1 回だけ」
 * ワーカーセッション (Claude Code / Codex) に注入され、 役割と動作プロトコルを
 * 固定する。 以降のターンは pool が /v1/keys で 1 行注入する。
 */

import type { PersonaSeed } from "../types.js";
import type { WorkerConfig } from "./types.js";

/** 既定の 8 キャスト。spec §3。 */
export const DEFAULT_WORKERS: WorkerConfig[] = [
  { id: "facilitator", role: "ファシリテーター", provider: "claude", model: "claude-opus-4-8" },
  { id: "pro-opus", role: "正論派", provider: "claude", model: "claude-opus-4-8" },
  { id: "con-opus", role: "否定派", provider: "claude", model: "claude-opus-4-8" },
  { id: "pro-gpt", role: "正論派", provider: "codex", model: "gpt-5.5" },
  { id: "con-gpt", role: "否定派", provider: "codex", model: "gpt-5.5" },
  { id: "opinion-opus", role: "意見屋", provider: "claude", model: "claude-opus-4-8" },
  { id: "opinion-sonnet", role: "意見屋", provider: "claude", model: "claude-sonnet-4-6" },
  { id: "opinion-gpt", role: "意見屋", provider: "codex", model: "gpt-5.5" },
];

/** モデル ID → 表示用の短縮タグ。 */
function modelShort(model: string): string {
  if (model.includes("opus")) return "Opus";
  if (model.includes("sonnet")) return "Sonnet";
  if (model.includes("gpt")) return "GPT";
  return model;
}

/**
 * ワーカー定義 → persona-engine の PersonaSeed。
 * persona id = worker id にして WorkerPoolClient のルーティングと一致させる。
 * provider/model は WorkerPool が config から知るので persona row には不要。
 */
export function buildWorkerPersonaSeeds(workers: WorkerConfig[]): PersonaSeed[] {
  return workers.map((w) => ({
    id: w.id,
    name: w.role,
    display_name: `${w.role}・${modelShort(w.model)}`,
    description: roleGuidance(w.role),
    traits: [w.role, w.provider, w.model],
    speech_style: "Discord の自然な口語。ラベルを付けず、一文ごとに改行。1〜3 文。",
  }));
}

/** 役割別の振る舞い指示。 */
function roleGuidance(role: string): string {
  switch (role) {
    case "ファシリテーター":
      return [
        "あなたは議論のファシリテーターです。対立する意見を整理し、止揚 (アウフヘーベン) を促し、",
        "人間の発言を最優先で拾う。結論を急がず、論点を一つに絞って次の一手を投げる。",
      ].join("\n");
    case "正論派":
      return [
        "あなたは正論派です。テーマの主張を筋の通った形で擁護・補強する。",
        "ただし盲信せず、根拠を添えて『なぜ妥当か』を一言で示す。",
      ].join("\n");
    case "否定派":
      return [
        "あなたは否定派です。健全な反対意見・反例・見落とされた弱点を投げかける。",
        "揚げ足取りではなく、議論を前に進める否定を心がける。",
      ].join("\n");
    case "意見屋":
      return [
        "あなたは意見屋です。賛否どちらにも寄りすぎず、自分の角度から独自の見方や具体例を出す。",
        "他のペルソナが触れていない切り口を一つ持ち込む。",
      ].join("\n");
    default:
      return `あなたは議論ペルソナ「${role}」です。役割に沿って発言する。`;
  }
}

/**
 * standing prompt を組み立てる。
 * - callbackBaseUrl: Discutere の base URL (例 http://127.0.0.1:3100)
 * - turnsDir: ターン JSON / 返信 JSON を置く相対ディレクトリ (cwd=Discutere 起点)
 */
export function buildStandingPrompt(args: {
  worker: WorkerConfig;
  callbackBaseUrl: string;
  turnsDir: string;
}): string {
  const { worker, callbackBaseUrl, turnsDir } = args;
  const cb = callbackBaseUrl.replace(/\/+$/, "");
  return [
    `# あなたの役割: ${worker.role} (worker id: ${worker.id})`,
    "",
    roleGuidance(worker.role),
    "",
    "## 口調ルール (厳守)",
    "- Discord で実在の人間が話すような自然な口語で書く。",
    "- 「反論:」「反例:」「弱点:」のようなラベルや見出しは絶対に付けない。普通の言葉で否定/賛成する。",
    "- 箇条書き・番号は使わず、会話の一言として書く。",
    "- 一文ごとに改行 (。! ? で改行)。1〜3 文程度。長文にしない。",
    "- 確信が無い、または発言する必要が無いと判断したら text を空文字にして skip してよい。",
    "",
    "## 動作プロトコル (重要 — このとおりに動く)",
    "",
    "### 1. 起動直後に一度だけ登録する",
    "次の Bash を今すぐ実行して、自分の Lictor port を登録してください:",
    "```bash",
    `curl -s -X POST ${cb}/internal/worker/register \\`,
    `  -H "content-type: application/json" \\`,
    `  -d "{\\"workerId\\":\\"${worker.id}\\",\\"lictorPort\\":$LICTOR_PORT}"`,
    "```",
    "登録できたら『登録完了』とだけ言って、次のターン指示を待ってください。",
    "",
    "### 2. ターンを受け取ったら発言する",
    "`[TURN] <reqId> <ファイルパス>` という 1 行が来たら、それがあなたの発言ターンです。",
    "手順:",
    `  a. 指定された JSON ファイル (${turnsDir}/<reqId>.json) を読む。中に system 指示と議論コンテキストがある。`,
    "  b. 役割と口調ルールに従って発言を 1 つ作る (1〜3 文)。発言しない場合は空文字。",
    `  c. 返信 JSON を ${turnsDir}/<reqId>.reply.json に書き出す。形式:`,
    `     {"reqId":"<reqId>","workerId":"${worker.id}","text":"<あなたの発言。skip 時は空文字>"}`,
    "  d. 次の Bash で送信する (日本語 mojibake 回避のため必ず --data-binary @file):",
    "```bash",
    `curl -s -X POST ${cb}/internal/worker/utterance \\`,
    `  -H "content-type: application/json" \\`,
    `  --data-binary @${turnsDir}/<reqId>.reply.json`,
    "```",
    "  e. 送信したら、それ以外は何もせず次のターンを静かに待つ。",
    "",
    "発言生成と送信以外の作業 (コード編集・調査・質問) はしないでください。",
  ].join("\n");
}
