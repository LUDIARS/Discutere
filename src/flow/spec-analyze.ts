/**
 * 仕様書の解析学習 (② 仕様書の解析学習, LLM)。
 *
 * 貼り付けられた**ゲーム仕様書テキスト**を LLM に渡し、語られている**遊びのメカニクス**を
 * `GameMechanicEntry[]` として抽出する。抽出結果は学習フロー (learning.ts) の mechanics 記録経路
 * (data/games/<slug>.md + Discatier Core) にそのまま流す → 議論フローの調査が読める。
 *
 * 仕様書は「ユーザの意見」ではなく**作者の設計文書**なので、外部の声 (opinions) ではなく
 * メカニクスに倒す (① 類似ゲームの自動収集が外部の声、② が設計メカニクス、という役割分担)。
 *
 * LLM 失敗 / JSON 解析失敗は空配列で degrade する (学習を止めない)。
 * DB 非依存・LLM 注入 — 単体テスト可能。JSON 抽出は mechanic-extract と共有 (extractJsonArray)。
 */

import type { LLMClient } from "../persona-engine/llm/client.js";
import type { GameMechanicEntry } from "./games-md.js";
import { extractJsonArray } from "./mechanic-extract.js";

/** 1 チャンクあたりの最大文字数。 */
const SPEC_CHAR_CAP = 16000;
/** チャンク間のオーバーラップ (文脈の途切れを和らげる)。 */
const CHUNK_OVERLAP = 2000;
/** 処理するチャンクの最大数 (LLM コスト上限)。 */
const MAX_CHUNKS = 4;

export interface AnalyzeSpecMechanicsArgs {
  /** ゲーム / テーマ名 (プロンプト文脈)。 */
  theme: string;
  /** 仕様書テキスト (Web 貼り付け / Discord starter 本文)。 */
  specText: string;
  llm: LLMClient;
  /** 抽出上限 (既定 40)。 */
  maxMechanics?: number;
  model?: string;
  warn?: (msg: string) => void;
}

const VALENCES = new Set(["positive", "negative", "neutral"]);

/** LLM 返答 1 件を GameMechanicEntry に正規化する (name 必須・型ガード)。 */
function normalizeEntry(raw: unknown): GameMechanicEntry | null {
  if (!raw || typeof raw !== "object") return null;
  const m = raw as Record<string, unknown>;
  const name = typeof m.name === "string" ? m.name.trim() : "";
  if (!name) return null;

  const entry: GameMechanicEntry = { name };
  if (typeof m.description === "string" && m.description.trim()) entry.description = m.description.trim();
  if (typeof m.intended_affect === "string" && m.intended_affect.trim())
    entry.intended_affect = m.intended_affect.trim();
  if (typeof m.intended_valence === "string") {
    const v = m.intended_valence.trim().toLowerCase();
    if (VALENCES.has(v)) entry.intended_valence = v;
  }
  const toStrArray = (v: unknown): string[] | undefined => {
    if (!Array.isArray(v)) return undefined;
    const arr = v.filter((s): s is string => typeof s === "string" && s.trim() !== "").map((s) => s.trim());
    return arr.length ? arr : undefined;
  };
  const aspects = toStrArray(m.intended_aspects);
  if (aspects) entry.intended_aspects = aspects;
  const emotions = toStrArray(m.intended_emotions);
  if (emotions) entry.intended_emotions = emotions;
  return entry;
}

/** name (lower) で重複除去し、target 件で切る。 */
function dedupe(entries: GameMechanicEntry[], target: number): GameMechanicEntry[] {
  const seen = new Set<string>();
  const out: GameMechanicEntry[] = [];
  for (const e of entries) {
    const key = e.name.trim().toLowerCase();
    if (seen.has(key)) continue;
    seen.add(key);
    out.push(e);
    if (out.length >= target) break;
  }
  return out;
}

function buildPrompt(theme: string, specText: string, max: number): { system: string; prompt: string } {
  const system =
    "あなたはゲームデザイン分析者です。 渡されたゲーム仕様書から、 実装されている / 設計されている" +
    " **遊びのメカニクス** (プレイヤーの行動とその設計意図) を構造化して洗い出します。" +
    " 仕様書に書かれている内容に忠実に、 具体的なメカニクスを抽出してください。 返答は JSON 配列のみ。";
  const prompt =
    `# ゲーム / テーマ\n${theme}\n\n# 仕様書\n${specText}\n\n` +
    `上記の仕様書から読み取れる遊びのメカニクスを最大 ${max} 件、 次の JSON 配列だけで返してください` +
    " (前後に説明やコードフェンスを付けない)。 各フィールドは仕様書から読み取れる範囲で埋め、" +
    " 不明な任意フィールドは省略してください:\n" +
    '[{"name":"メカニクス名","description":"プレイヤーの行動と設計意図 (1〜2文)",' +
    '"intended_affect":"狙う感情 (任意)","intended_valence":"positive|negative|neutral (任意)",' +
    '"intended_aspects":["設計面 (任意)"],"intended_emotions":["想定感情 (任意)"]}]';
  return { system, prompt };
}

/** 仕様書テキストを SPEC_CHAR_CAP 文字のチャンクに分割する (CHUNK_OVERLAP の重複あり)。 */
function splitIntoChunks(text: string): string[] {
  if (text.length <= SPEC_CHAR_CAP) return [text];
  const chunks: string[] = [];
  const step = SPEC_CHAR_CAP - CHUNK_OVERLAP;
  for (let i = 0; i < text.length && chunks.length < MAX_CHUNKS; i += step) {
    chunks.push(text.slice(i, i + SPEC_CHAR_CAP));
  }
  return chunks;
}

/** 1 チャンクを LLM で解析し GameMechanicEntry[] を返す。失敗時は [] で degrade。 */
async function analyzeChunk(
  theme: string,
  chunk: string,
  chunkIndex: number,
  args: AnalyzeSpecMechanicsArgs
): Promise<GameMechanicEntry[]> {
  const { llm, model, maxMechanics = 40, warn = () => {} } = args;
  let res;
  try {
    const { system, prompt } = buildPrompt(theme, chunk, maxMechanics);
    const maxTokens = Math.min(8000, Math.max(2000, maxMechanics * 300));
    res = await llm.invoke({ system, prompt, model, maxTokens });
  } catch (e) {
    warn(`仕様書解析 (チャンク${chunkIndex + 1}) で例外: ${(e as Error).message}`);
    return [];
  }
  if (!res.ok) {
    warn(`仕様書解析 (チャンク${chunkIndex + 1}) に失敗: ${res.error}`);
    return [];
  }
  const arr = extractJsonArray(res.text);
  if (!arr) {
    warn(`仕様書解析 (チャンク${chunkIndex + 1}) の JSON 解析に失敗`);
    return [];
  }
  return arr.map(normalizeEntry).filter((e): e is GameMechanicEntry => e !== null);
}

/**
 * 仕様書テキストから遊びのメカニクスを LLM で抽出する。
 * specText が空なら LLM を呼ばず []。
 * SPEC_CHAR_CAP 文字超の場合はチャンク分割して各チャンクを解析・マージする (最大 MAX_CHUNKS チャンク)。
 * 失敗時も [] で degrade (学習を止めない)。
 */
export async function analyzeSpecMechanics(args: AnalyzeSpecMechanicsArgs): Promise<GameMechanicEntry[]> {
  const { theme, maxMechanics = 40, warn = () => {} } = args;
  const specText = args.specText?.trim() ?? "";
  if (!specText) return [];

  const chunks = splitIntoChunks(specText);
  if (chunks.length > 1) {
    warn(
      `仕様書が長いため ${chunks.length} チャンクに分割して解析します (元 ${specText.length} 文字、` +
      `最大 ${MAX_CHUNKS * SPEC_CHAR_CAP} 文字を対象)`
    );
  }

  const allEntries: GameMechanicEntry[] = [];
  for (let i = 0; i < chunks.length; i++) {
    const entries = await analyzeChunk(theme, chunks[i]!, i, args);
    allEntries.push(...entries);
  }
  return dedupe(allEntries, maxMechanics);
}
