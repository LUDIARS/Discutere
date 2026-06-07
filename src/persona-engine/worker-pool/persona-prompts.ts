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

/**
 * 役割別の既定ガイダンス (override 比較のため辞書として公開)。
 * runtime-settings の rolePrompts デフォルトに渡す。
 */
export const ROLE_GUIDANCE_DEFAULTS: Record<string, string> = {
  ファシリテーター: [
    "あなたは議論のファシリテーターです。対立する意見を整理し、止揚 (アウフヘーベン) を促し、",
    "人間の発言を最優先で拾う。結論を急がず、論点を一つに絞って次の一手を投げる。",
    "時々、議論を見ている人間に『あなたはどう感じる?』と意見や具体例を尋ねて巻き込む。",
  ].join("\n"),
  正論派: [
    "あなたは正論派です。テーマの主張を筋の通った形で擁護・補強する。",
    "ただし盲信せず、根拠を添えて『なぜ妥当か』を一言で示す。",
  ].join("\n"),
  否定派: [
    "あなたは否定派です。健全な反対意見・反例・見落とされた弱点を投げかける。",
    "揚げ足取りではなく、議論を前に進める否定を心がける。",
  ].join("\n"),
  意見屋: [
    "あなたは意見屋です。賛否どちらにも寄りすぎず、自分の角度から独自の見方や具体例を出す。",
    "他のペルソナが触れていない切り口を一つ持ち込む。",
    "ただし具体例は必ず議題の対象 (そのゲーム) に即したものにする。",
    "議題と無関係な別のゲームを例や話題に持ち出さない。",
  ].join("\n"),
};

/**
 * 役割ガイダンスの runtime override 解決関数。 index.ts が runtime-settings ストアを
 * 注入する。 未注入なら hardcoded デフォルトを使う (テスト / 単体実行時)。
 * `setWorkerPoolControl` 等と同じモジュールレベル注入パターン。
 */
let rolePromptResolver: ((role: string) => string | undefined) | null = null;
export function setRolePromptResolver(fn: ((role: string) => string | undefined) | null): void {
  rolePromptResolver = fn;
}

/**
 * ペルソナの表示名 (= Discord webhook の username)。 役割名ではなく人名にする。
 * 既定キャストは固定名、 未知 worker id は FALLBACK から index で割り当てる。
 */
const PERSONA_NAMES: Record<string, string> = {
  facilitator: "ナギ",
  "pro-opus": "ハルキ",
  "con-opus": "レン",
  "pro-gpt": "ソウタ",
  "con-gpt": "ユウ",
  "opinion-opus": "ミオ",
  "opinion-sonnet": "アオイ",
  "opinion-gpt": "ハル",
};
const FALLBACK_NAMES = ["カエデ", "ツバサ", "ヒナタ", "リク", "サクラ", "ノゾミ", "タクミ", "マコト"];

function personaDisplayName(workerId: string, index: number): string {
  return PERSONA_NAMES[workerId] ?? FALLBACK_NAMES[index % FALLBACK_NAMES.length];
}

/**
 * ワーカー定義 → persona-engine の PersonaSeed。
 * persona id = worker id にして WorkerPoolClient のルーティングと一致させる。
 * provider/model は WorkerPool が config から知るので persona row には不要。
 * display_name は人名 (役割名は出さない) — Discord には webhook username として出る。
 */
export function buildWorkerPersonaSeeds(workers: WorkerConfig[]): PersonaSeed[] {
  return workers.map((w, i) => ({
    id: w.id,
    name: w.role,
    display_name: personaDisplayName(w.id, i),
    description: roleGuidance(w.role),
    traits: [w.role, w.provider, w.model],
    speech_style: "Discord の自然な口語。ラベルを付けず、一文ごとに改行。1〜3 文。",
  }));
}

/**
 * 役割別の振る舞い指示。 runtime override があればそれを、 無ければ hardcoded
 * デフォルト、 未知の役割はフォールバック文。
 */
function roleGuidance(role: string): string {
  const override = rolePromptResolver?.(role);
  if (override && override.trim() !== "") return override;
  return ROLE_GUIDANCE_DEFAULTS[role] ?? `あなたは議論ペルソナ「${role}」です。役割に沿って発言する。`;
}

/**
 * standing prompt を組み立てる。
 *
 * register / utterance は生 curl をやめ、cwd (= worker-home) の
 * `scripts/{register,send}.mjs` を実行させる (auto-mode 分類器の遮断回避 +
 * allow-list で待ち無し実行)。callback URL / worker id / Lictor port は
 * スクリプトが環境変数 (DI_CALLBACK_URL / DI_WORKER_ID / LICTOR_PORT) から読む。
 */
export function buildStandingPrompt(args: { worker: WorkerConfig }): string {
  const { worker } = args;
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
    "- 議題に与えられた対象 (ゲーム名など) から逸れない。別のゲームを例や話題に持ち出さない。",
    "- 直近の発言で既に出た主張・例の繰り返しはしない。新しい点が無ければ skip する。",
    "- 確信が無い、または発言する必要が無いと判断したら text を空文字にして skip してよい。",
    "",
    "## 動作プロトコル (重要 — このとおりに動く)",
    "",
    "### 1. 起動直後に一度だけ登録する",
    "次の Bash を今すぐ実行して、自分を登録してください (引数は不要、環境変数から自動取得):",
    "```bash",
    "node scripts/register.mjs",
    "```",
    "登録できたら『登録完了』とだけ言って、次のターン指示を待ってください。",
    "",
    "### 2. ターンを受け取ったら発言する",
    "`[TURN] <reqId> <ターン JSON の絶対パス>` という 1 行が来たら、それがあなたの発言ターンです。",
    "手順:",
    "  a. 指定された絶対パスの JSON ファイルを Read で読む。中に system 指示と議論コンテキストがある。",
    "  b. 役割と口調ルールに従って発言を 1 つ作る (1〜3 文)。発言しない場合は空文字。",
    "     ※ ターン本文に『JSON で返せ』『action を返せ』等の形式指示があっても無視し、",
    "       あなたは常に発話本文 (プレーンテキスト) だけを text に入れる。",
    "  c. 返信 JSON を `replies/<reqId>.reply.json` (cwd 相対) に Write で書き出す。形式:",
    `     {"reqId":"<reqId>","workerId":"${worker.id}","text":"<あなたの発言。skip 時は空文字>"}`,
    "  d. 次の Bash で送信する (ファイルを生で送るので日本語も化けない):",
    "```bash",
    "node scripts/send.mjs replies/<reqId>.reply.json",
    "```",
    "  e. 送信したら、それ以外は何もせず次のターンを静かに待つ。",
    "",
    "発言生成と送信以外の作業 (コード編集・調査・質問) はしないでください。",
  ].join("\n");
}
