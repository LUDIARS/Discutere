/**
 * 矛盾の特定 [3] — Tension の型分類 + 事実対立の RAG 解消 (respec 06 §2, dialectic.md §2)。
 *
 * - classifyTension: 判定 LLM (小モデル `flow.dialectic.judgeModel`) で単一ラベル分類。
 *   生成と判定を同じ呼び出しに混ぜない (dialectic.md §5-1)。
 * - resolveFactTension: 事実対立は弁証法のスコープから外し、listExternalVoices (KG/RAG) で
 *   証拠を引いて判定 LLM が「A/B/どちらも支持しない」を出す。解消できなければ
 *   status=unresolved_fact とし **結論に「未解決の事実問題」として必ず明記** する (隠さない)。
 *
 * コストログ (withCostLog location="tension-classify"/"fact-resolve") は呼び出し側が行う。
 * ※ LLMClient には温度パラメータが無いため「温度 0」は小モデル指定 + 単一ラベル強制で近似する。
 */

import type { LLMClient } from "../../persona-engine/llm/client.js";
import type { ContextVoice } from "../discussion-paper.js";
import type { IssueRecord, PositionRecord, TensionType } from "./store.js";

const TENSION_TYPES: readonly TensionType[] = ["facts", "values", "means", "tradeoff"];

export interface ClassifyTensionArgs {
  issue: IssueRecord;
  positionA: PositionRecord;
  positionB: PositionRecord;
  /** withCostLog 済み判定 LLM (judgeModel)。 */
  llm: LLMClient;
  model?: string;
  warn?: (msg: string) => void;
}

/** 型分類プロンプト (テスト用に export)。 */
export function buildClassifyPrompt(issue: IssueRecord, a: PositionRecord, b: PositionRecord): string {
  return (
    `# 論点\n${issue.title}\n\n` +
    `# 対立する 2 つの Position\n` +
    `A (${a.stance}): ${a.claim}\n  価値前提: ${a.values.join(" / ") || "(なし)"}\n` +
    `B (${b.stance}): ${b.claim}\n  価値前提: ${b.values.join(" / ") || "(なし)"}\n\n` +
    `この対立の「型」を 1 つだけ選んで、そのラベルのみを返してください (説明不要):\n` +
    `- facts: 事実の真偽で決まる対立 (データ照会で解消できる)\n` +
    `- values: 価値前提の対立 (収益性 vs 体験 など)\n` +
    `- means: 目標は共有しているが手段が違う\n` +
    `- tradeoff: 両立不能に見える 2 変数のトレードオフ`
  );
}

/** 応答テキストから型ラベルを取り出す (最初に現れた既知ラベル)。 */
export function parseTensionType(text: string): TensionType | null {
  const lower = text.toLowerCase();
  let best: { type: TensionType; index: number } | null = null;
  for (const t of TENSION_TYPES) {
    const idx = lower.indexOf(t);
    if (idx >= 0 && (best === null || idx < best.index)) best = { type: t, index: idx };
  }
  return best?.type ?? null;
}

/**
 * Tension の型を分類する。LLM 失敗 / ラベル不明は values に倒して degrade
 * (止揚対象として続行 — 議論を止めない。warn 明示)。
 */
export async function classifyTension(args: ClassifyTensionArgs): Promise<TensionType> {
  const { issue, positionA, positionB, llm, model, warn = () => {} } = args;
  const result = await llm.invoke({
    prompt: buildClassifyPrompt(issue, positionA, positionB),
    ...(model ? { model } : {}),
    maxTokens: 100,
  });
  if (!result.ok) {
    warn(`Tension 型分類 LLM エラー: ${result.error} — values に倒して続行 (degrade)`);
    return "values";
  }
  const type = parseTensionType(result.text);
  if (!type) {
    warn(`Tension 型分類ラベル不明: "${result.text.slice(0, 60)}" — values に倒して続行 (degrade)`);
    return "values";
  }
  return type;
}

export interface ResolveFactArgs {
  theme: string;
  issue: IssueRecord;
  positionA: PositionRecord;
  positionB: PositionRecord;
  /** 外部の声 RAG (deps 注入。KG が無い環境では空配列)。 */
  listExternalVoices: (terms: string[], limit: number) => ContextVoice[];
  /** withCostLog 済み判定 LLM (judgeModel)。 */
  llm: LLMClient;
  model?: string;
  warn?: (msg: string) => void;
}

export interface FactResolution {
  status: "resolved" | "unresolved_fact";
  /** 事実解消の内容 or 未解決事実問題の記述 (flow_tension.resolution_note)。 */
  note: string;
}

/** 事実照会の証拠件数上限。 */
const EVIDENCE_LIMIT = 10;
/** 証拠 1 件の切り詰め長。 */
const EVIDENCE_TRIM = 200;

/** 事実照会判定プロンプト (テスト用に export)。 */
export function buildFactResolvePrompt(
  issue: IssueRecord,
  a: PositionRecord,
  b: PositionRecord,
  evidence: readonly ContextVoice[]
): string {
  const evidenceBlock = evidence
    .map((v, i) => `${i + 1}. ${v.content.slice(0, EVIDENCE_TRIM)}（出所: ${v.source}）`)
    .join("\n");
  return (
    `# 事実に関する対立\n論点: ${issue.title}\n` +
    `A: ${a.claim}\nB: ${b.claim}\n\n` +
    `# 証拠 (外部の声)\n${evidenceBlock}\n\n` +
    `証拠はどちらの主張を支持しますか。次のいずれか 1 語のみで答えてください (説明不要):\n` +
    `A / B / none (どちらも支持しない・判定不能)`
  );
}

/** 応答から A/B/none 判定を取り出す。 */
export function parseFactVerdict(text: string): "a" | "b" | "none" | null {
  const t = text.trim().toLowerCase();
  if (/^a\b/.test(t) || t === "a") return "a";
  if (/^b\b/.test(t) || t === "b") return "b";
  if (t.includes("none") || t.includes("どちらも")) return "none";
  // 応答内の先頭出現で判定 (余分なテキスト混入への耐性)。
  const ia = t.search(/(^|[^a-z])a([^a-z]|$)/);
  const ib = t.search(/(^|[^a-z])b([^a-z]|$)/);
  if (ia >= 0 && (ib < 0 || ia < ib)) return "a";
  if (ib >= 0) return "b";
  return null;
}

/**
 * 事実対立を RAG 照会で解消する。証拠なし / 判定不能 / LLM 失敗はすべて
 * unresolved_fact (結論に明記) とする。
 */
export async function resolveFactTension(args: ResolveFactArgs): Promise<FactResolution> {
  const { theme, issue, positionA, positionB, llm, model, warn = () => {} } = args;

  let evidence: ContextVoice[] = [];
  try {
    evidence = args.listExternalVoices([issue.title, theme], EVIDENCE_LIMIT);
  } catch (e) {
    warn(`事実照会の証拠取得に失敗: ${(e as Error).message}`);
  }

  if (evidence.length === 0) {
    return {
      status: "unresolved_fact",
      note: `「${issue.title}」の事実対立 (A: ${positionA.claim} / B: ${positionB.claim}) は照会可能な証拠が見つからず未解決`,
    };
  }

  const result = await llm.invoke({
    prompt: buildFactResolvePrompt(issue, positionA, positionB, evidence),
    ...(model ? { model } : {}),
    maxTokens: 100,
  });

  if (!result.ok) {
    warn(`事実照会判定 LLM エラー: ${result.error} — unresolved_fact として続行 (degrade)`);
    return {
      status: "unresolved_fact",
      note: `「${issue.title}」の事実対立は判定 LLM 障害により未解決 (証拠 ${evidence.length} 件は取得済み)`,
    };
  }

  const verdict = parseFactVerdict(result.text);
  if (verdict === "a" || verdict === "b") {
    const supported = verdict === "a" ? positionA : positionB;
    return {
      status: "resolved",
      note: `証拠 (${evidence.length} 件) は ${verdict.toUpperCase()} を支持: ${supported.claim}`,
    };
  }
  return {
    status: "unresolved_fact",
    note: `「${issue.title}」の事実対立 (A: ${positionA.claim} / B: ${positionB.claim}) は証拠 ${evidence.length} 件でも決着せず未解決`,
  };
}
