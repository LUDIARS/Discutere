/**
 * 0+1 感情カスケード — spec/crawler/EXTERNAL-SOURCES.md §§4.2-4.3 参照。
 *
 * Tier 0: 辞書 (lexicon.json) + 信号 (upvotes/votedUp)。決定論的・無コスト。
 * Tier 1: ローカル LLM (Ollama/Gemma 等の OpenAI 互換)。Tier 0 が不確かな時のみ。
 * Residual: メイン LLM (Claude API/CLI)。Tier 1 でも不確かな場合のみ。
 *
 * テキスト 1 件 → SentimentResult。呼び出し元がバッチループする。
 */

import { createRequire } from "node:module";
import { fileURLToPath } from "node:url";
import path from "node:path";
import type { LLMClient } from "../../persona-engine/llm/client.js";

const _require = createRequire(import.meta.url);
const _here = path.dirname(fileURLToPath(import.meta.url));

// lexicon.json は .mjs と同じディレクトリに置かれている
const LEX = _require(path.join(_here, "lexicon.json")) as Lexicon;

interface Lexicon {
  polarity: Record<string, number>;
}

export type Polarity = "positive" | "negative" | "neutral";

export interface SentimentResult {
  polarity: Polarity;
  /** 極性スコア -1..1 (正 = positive) */
  score: number;
  /** 確信度 0..1 */
  confidence: number;
  /** 解決した tier (0=辞書, 1=ローカルLLM, 2=メインLLM) */
  tier: 0 | 1 | 2;
}

export interface Signal {
  upvotes?: number;
  votedUp?: boolean;
}

/** Tier 0 閾値: |score| > これなら tier 0 で確定。 */
const T0_THRESHOLD = 0.3;
/** Tier 1 閾値: LLM の confidence がこれ以上なら確定。 */
const T1_THRESHOLD = 0.65;

// ─── Tier 0: 辞書 + 信号 ───────────────────────────────────────────────────

function tier0Score(text: string, signal?: Signal): number {
  const t = text.toLowerCase();
  let pol = 0;
  let hits = 0;
  for (const [word, sc] of Object.entries(LEX.polarity)) {
    if (t.includes(word.toLowerCase())) {
      pol += sc;
      hits++;
    }
  }
  if (signal?.votedUp === true) { pol += 1.5; hits++; }
  else if (signal?.votedUp === false) { pol -= 1.5; hits++; }
  // upvotes の対数で重みを付ける (上限 1.0)
  if (signal?.upvotes && signal.upvotes > 0) {
    const boost = Math.min(1.0, Math.log10(signal.upvotes + 1) / 3);
    pol += boost * (hits > 0 ? Math.sign(pol) : 0);
  }
  return hits > 0 ? Math.max(-1, Math.min(1, pol / Math.max(2, hits))) : 0;
}

function tier0(text: string, signal?: Signal): SentimentResult | null {
  const score = tier0Score(text, signal);
  const confidence = Math.abs(score);
  if (confidence < T0_THRESHOLD) return null; // 不確か → escalate
  return {
    polarity: score > 0 ? "positive" : "negative",
    score,
    confidence,
    tier: 0,
  };
}

// ─── Tier 1: ローカル LLM ──────────────────────────────────────────────────

const T1_SYSTEM =
  "You are a sentiment classifier for game reviews/comments. " +
  "Reply ONLY with a JSON object: {\"polarity\":\"positive\"|\"negative\"|\"neutral\",\"confidence\":0.0..1.0}. " +
  "No other text.";

function buildT1Prompt(text: string): string {
  return `Classify the sentiment of this game review/comment:\n\n"${text.slice(0, 300)}"`;
}

function parseT1Response(raw: string): { polarity: Polarity; confidence: number } | null {
  try {
    const m = raw.match(/\{[\s\S]*\}/);
    if (!m) return null;
    const obj = JSON.parse(m[0]) as Record<string, unknown>;
    const polarity = obj.polarity as Polarity;
    const confidence = Number(obj.confidence ?? 0);
    if (!["positive", "negative", "neutral"].includes(polarity)) return null;
    return { polarity, confidence: Math.max(0, Math.min(1, confidence)) };
  } catch {
    return null;
  }
}

async function tier1(text: string, llm: LLMClient): Promise<SentimentResult | null> {
  const res = await llm.invoke({ system: T1_SYSTEM, prompt: buildT1Prompt(text), maxTokens: 64 });
  if (!res.ok) return null;
  const parsed = parseT1Response(res.text);
  if (!parsed || parsed.confidence < T1_THRESHOLD) return null;
  const score = parsed.polarity === "positive" ? parsed.confidence : parsed.polarity === "negative" ? -parsed.confidence : 0;
  return { polarity: parsed.polarity, score, confidence: parsed.confidence, tier: 1 };
}

// ─── Residual LLM ──────────────────────────────────────────────────────────

async function residual(text: string, llm: LLMClient): Promise<SentimentResult> {
  const res = await llm.invoke({ system: T1_SYSTEM, prompt: buildT1Prompt(text), maxTokens: 64 });
  if (!res.ok) return { polarity: "neutral", score: 0, confidence: 0, tier: 2 };
  const parsed = parseT1Response(res.text);
  if (!parsed) return { polarity: "neutral", score: 0, confidence: 0, tier: 2 };
  const score = parsed.polarity === "positive" ? parsed.confidence : parsed.polarity === "negative" ? -parsed.confidence : 0;
  return { polarity: parsed.polarity, score, confidence: parsed.confidence, tier: 2 };
}

// ─── カスケード本体 ────────────────────────────────────────────────────────

export interface CascadeClients {
  /** Tier 1 ローカル LLM。未設定時は Tier 1 をスキップして Residual へ。 */
  local?: LLMClient;
  /** Residual メイン LLM。未設定時は neutral を返す。 */
  main?: LLMClient;
}

/**
 * テキスト 1 件の感情を 0+1 カスケードで判定する。
 * 呼び出し元がバッチで回す設計 (バッチ化はここでは行わない)。
 */
export async function cascadeSentiment(
  text: string,
  signal: Signal | undefined,
  clients: CascadeClients,
): Promise<SentimentResult> {
  if (!text.trim()) return { polarity: "neutral", score: 0, confidence: 0, tier: 0 };

  // Tier 0
  const r0 = tier0(text, signal);
  if (r0) return r0;

  // Tier 1
  if (clients.local) {
    const r1 = await tier1(text, clients.local);
    if (r1) return r1;
  }

  // Residual
  if (clients.main) return residual(text, clients.main);

  // フォールバック: Tier 0 の弱いスコアを返す
  const score = tier0Score(text, signal);
  return {
    polarity: score > 0.05 ? "positive" : score < -0.05 ? "negative" : "neutral",
    score,
    confidence: Math.abs(score),
    tier: 0,
  };
}
