/**
 * dialectic エンジンの発話プロンプト + パース (respec 05 §1)。
 *
 * ペルソナターンは露出用の口語文と論証上の「手」(act) を同時に返す:
 *   { "act": "...", "target": "<utterance id>", "groundId": "<根拠 id>", "text": "..." }
 *
 * - act の選択肢はスケジューラが指名時に制限する (応答権指名なら rebut/support/concede/question)。
 * - パース失敗は degrade: 全文を act="claim", target=null として受理 + warn (発話を捨てない。
 *   OVERVIEW §10)。露出面には text のみ流す。
 * - 状態はダイジェストで渡す (dialectic.md §5-3): 当該 issue の Position 2 つ +
 *   未解決 challenge だけを user 側に注入する (ペーパー base は system に固定)。
 */

import type { FlowPersona, FlowStance } from "../personas.js";
import { extractJsonObject } from "../effect-predict.js";
import type { DialogueAct, RebuttalEdge } from "../argument-graph.js";
import { coerceAct } from "../argument-graph.js";
import { stanceLine } from "../discussion-paper.js";
import type { IssueRecord, PositionRecord } from "./store.js";

/** ターゲット表示用の発話 lookup (id → 発話主体名 + 本文)。 */
export interface UtteranceDigest {
  id: string;
  personaName: string;
  text: string;
}

export interface DialecticTurnPromptArgs {
  persona: FlowPersona;
  stance: FlowStance;
  issue: IssueRecord;
  /** 当該 issue の Position (定立/反定立 のダイジェスト)。 */
  positions: readonly PositionRecord[];
  /** 未解決の challenge (graph.unresolvedRebuttals のうち当該 issue ぶん)。 */
  unresolvedRebuttals: readonly RebuttalEdge[];
  /** id → 発話 (ターゲットの本文提示用)。 */
  utterances: ReadonlyMap<string, UtteranceDigest>;
  /** persona id → 表示名 (Position の持ち主表示用)。 */
  personaNames: ReadonlyMap<string, string>;
  /** スケジューラが許可した act 集合 (指名制)。 */
  allowedActs: readonly DialogueAct[];
  /** 応答権指名のターゲット (これに応答せよ)。自由ターンは undefined。 */
  targetUtteranceId?: string;
  /** ファシリテーターの指名文 (露出済み)。プロンプトにも文脈として載せる。 */
  nomination?: string;
}

/** Position をダイジェスト行にする (根拠は id 付き = rebut/批准のターゲット指定用)。 */
export function renderPositionDigest(p: PositionRecord, personaName: string): string {
  const grounds = p.grounds
    .map((g) => `  - [${g.id}] (${g.state}) ${g.text}`)
    .join("\n");
  const values = p.values.length > 0 ? `\n  価値前提: ${p.values.join(" / ")}` : "";
  return `${personaName} (${p.stance}): ${p.claim}\n${grounds}${values}`;
}

/** dialectic ターンの user プロンプトを組み立てる (system はペーパー base 固定)。 */
export function buildDialecticTurnPrompt(args: DialecticTurnPromptArgs): string {
  const { persona, stance, issue, positions, unresolvedRebuttals, utterances, allowedActs } = args;

  const positionBlock = positions
    .map((p) => {
      const ownerName = args.personaNames.get(p.personaId) ?? p.personaId;
      return renderPositionDigest(p, p.personaId === persona.id ? `${ownerName} (あなた)` : ownerName);
    })
    .join("\n\n");

  const unresolvedBlock =
    unresolvedRebuttals.length > 0
      ? unresolvedRebuttals
          .map((e) => {
            const rebut = utterances.get(e.rebutId);
            const target = utterances.get(e.targetId);
            return `- [${e.rebutId}] ${rebut ? `${rebut.personaName}: ${rebut.text.slice(0, 120)}` : "(反論)"}` +
              (target ? ` → 対象: ${target.personaName} の主張` : "");
          })
          .join("\n")
      : "(未応答の反論なし)";

  const targetBlock = args.targetUtteranceId
    ? (() => {
        const t = utterances.get(args.targetUtteranceId!);
        return t
          ? `# 応答対象\n[${t.id}] ${t.personaName}: ${t.text}\nこの発言に応答してください。\n`
          : "";
      })()
    : "";

  const nominationLine = args.nomination ? `進行役: ${args.nomination}\n` : "";

  const actList = allowedActs.join(" | ");

  return [
    `あなたは議論ペルソナ「${persona.name}」。`,
    `特徴: ${persona.traits.join(" / ")} / 話し方: ${persona.speechStyle}`,
    ...(persona.valueAxis ? [`あなたが重視する価値: ${persona.valueAxis}`] : []),
    stanceLine(stance),
    "",
    `# 論点 ${issue.ordinal}`,
    issue.title,
    "",
    `# 現在の論証状態 (根拠は [id] で指す)`,
    positionBlock || "(Position なし)",
    "",
    `# 未応答の反論`,
    unresolvedBlock,
    "",
    nominationLine + targetBlock +
      `# 指示\n` +
      `あなたの発言は次の JSON 1 個だけで返す (前後に説明やコードフェンスを付けない):\n` +
      `{"act": "<${actList}>", "target": "<応答先 utterance id (claim/question は null 可)>", ` +
      `"groundId": "<rebut のとき攻撃対象の根拠 id (任意)>", ` +
      `"text": "<Discord に流す口語 1〜3 文>"}\n` +
      `act は今の議論状態で最も議論を前に進める手を選ぶ (許可: ${actList})。\n` +
      `text は実在の人間の雑談のような自然な口語で書く (ラベル・箇条書き禁止)。`,
  ].join("\n");
}

/** パース済みのターン応答。 */
export interface ParsedTurn {
  act: DialogueAct;
  targetId: string | null;
  /** rebut の攻撃対象根拠 id (LLM 申告。無ければ null → 呼び出し側が決定的に選ぶ)。 */
  groundId: string | null;
  text: string;
  /** JSON が取れず全文 claim として受理した (degrade) か。 */
  degraded: boolean;
}

export interface ParseTurnOptions {
  allowedActs: readonly DialogueAct[];
  /** 有効な応答先 id 集合 (これ以外の target は捨てる)。 */
  validTargets: ReadonlySet<string>;
  /** 応答権指名時の既定ターゲット (target 欠落/不正時のフォールバック)。 */
  defaultTargetId?: string | null;
  warn?: (msg: string) => void;
}

/**
 * ターン応答をパースする。JSON 崩れは act=claim, target=null で全文受理 + warn
 * (発話を捨てない)。act が許可外なら許可集合の先頭に矯正する (指名制の強制)。
 */
export function parseTurnResponse(raw: string, opts: ParseTurnOptions): ParsedTurn {
  const warn = opts.warn ?? (() => {});
  const fallbackText = raw.trim();

  const obj = extractJsonObject(raw);
  if (!obj || typeof obj.text !== "string" || !obj.text.trim()) {
    warn(`発話 JSON パース失敗 — 全文を act=claim として受理 (degrade): ${fallbackText.slice(0, 60)}`);
    return { act: "claim", targetId: null, groundId: null, text: fallbackText, degraded: true };
  }

  let act = coerceAct(typeof obj.act === "string" ? obj.act : null);
  if (!opts.allowedActs.includes(act)) {
    const forced = opts.allowedActs[0] ?? "claim";
    warn(`act "${act}" は許可外 (許可: ${opts.allowedActs.join(",")}) — "${forced}" に矯正`);
    act = forced;
  }

  let targetId: string | null = null;
  const rawTarget = typeof obj.target === "string" ? obj.target.trim() : "";
  if (rawTarget && opts.validTargets.has(rawTarget)) {
    targetId = rawTarget;
  } else {
    if (rawTarget) warn(`target "${rawTarget}" は不明な utterance id — 指名ターゲットに置換`);
    targetId = opts.defaultTargetId ?? null;
  }
  // 応答系 act なのに target が無い場合も指名ターゲットへフォールバック。
  if (!targetId && (act === "rebut" || act === "support" || act === "concede")) {
    targetId = opts.defaultTargetId ?? null;
  }

  const groundId = typeof obj.groundId === "string" && obj.groundId.trim() ? obj.groundId.trim() : null;

  return { act, targetId, groundId, text: obj.text.trim(), degraded: false };
}
