/**
 * ペルソナ合成 (F)。
 *
 * 2 体以上の親ペルソナを「ローグライク好き × ソシャゲユーザー」のように融合し、
 * 新しい永続ペルソナを生成・保存する。
 *  - affect ベクトル: 親ベクトルの (重み付き) 平均。
 *  - 人物像 (name / 口調 / traits): LLM で融合 (JSON)。失敗時は機械的フォールバック。
 * 保存後はプールの一員として憑依 (B) / 壁打ち相手 (G) / 通常議論に使える。
 */

import { randomUUID } from "node:crypto";
import type { LLMClient } from "../persona-engine/llm/client.js";
import { DIM } from "./sentiment-vector.js";
import {
  findPoolPersona,
  insertPoolPersona,
  type PoolPersona,
} from "./persona-pool.js";

/** 親ベクトルの重み付き平均 (要素ごと)。weights 省略時は均等。 */
export function averageVectors(vectors: number[][], weights?: number[]): number[] {
  if (vectors.length === 0) throw new Error("averageVectors: 親ベクトルが空です");
  for (const v of vectors) {
    if (v.length !== DIM) throw new Error(`averageVectors: dim mismatch (${v.length} != ${DIM})`);
  }
  const w = weights && weights.length === vectors.length ? weights : vectors.map(() => 1);
  const wsum = w.reduce((s, x) => s + x, 0) || 1;
  const out = new Array<number>(DIM).fill(0);
  vectors.forEach((v, i) => {
    for (let d = 0; d < DIM; d++) out[d] += (v[d] * w[i]) / wsum;
  });
  return out.map((x) => +x.toFixed(4));
}

/** LLM 応答から最初の JSON オブジェクトを寛容に取り出す。 */
function extractJson(text: string): Record<string, unknown> | null {
  const m = text.match(/\{[\s\S]*\}/);
  if (!m) return null;
  try {
    return JSON.parse(m[0]) as Record<string, unknown>;
  } catch {
    return null;
  }
}

export interface SynthesizeArgs {
  /** 親ペルソナの id または name (2 体以上)。 */
  parentRefs: string[];
  llm: LLMClient;
  weights?: number[];
  /** ラベル (省略時は親名の連結)。 */
  label?: string;
  warn?: (msg: string) => void;
}

/**
 * 親ペルソナを融合した新ペルソナを生成し、プールに保存して返す。
 * 親が 2 体未満 / 解決不能ならエラー。
 */
export async function synthesizePersona(args: SynthesizeArgs): Promise<PoolPersona> {
  const warn = args.warn ?? ((m) => console.warn(`[synthesize/warn] ${m}`));
  const parents: PoolPersona[] = [];
  for (const ref of args.parentRefs) {
    const p = findPoolPersona(ref);
    if (p) parents.push(p);
    else warn(`親ペルソナ「${ref}」が見つかりません`);
  }
  if (parents.length < 2) {
    throw new Error(`ペルソナ合成には解決可能な親が 2 体以上必要です (解決 ${parents.length})`);
  }

  const affectVector = averageVectors(parents.map((p) => p.affectVector), args.weights);

  // 人物像を LLM で融合 (JSON)。失敗時は機械フォールバック。
  const profiles = parents
    .map((p, i) => `親${i + 1}: 名前=${p.name} / 口調=${p.speechStyle} / 特徴=${p.traits.join("・")}`)
    .join("\n");
  const prompt =
    `次の複数の人物像を融合した、ゲームについて語る新しい 1 人の人物を作ってください。\n` +
    `${profiles}\n\n` +
    `両者の嗜好を併せ持つ自然な 1 人として、JSON のみで返答:\n` +
    `{"name":"<日本語の人物名>","speechStyle":"<話し方の短い説明>","traits":["<特徴>", "..."]}`;

  let name = parents.map((p) => p.name).join("×");
  let speechStyle = parents[0].speechStyle;
  let traits = [...new Set(parents.flatMap((p) => p.traits))];

  try {
    const res = await args.llm.invoke({ prompt, maxTokens: 300 });
    if (res.ok) {
      const j = extractJson(res.text);
      if (j) {
        if (typeof j.name === "string" && j.name.trim()) name = j.name.trim();
        if (typeof j.speechStyle === "string" && j.speechStyle.trim()) speechStyle = j.speechStyle.trim();
        if (Array.isArray(j.traits)) {
          const t = j.traits.filter((x): x is string => typeof x === "string" && x.trim() !== "");
          if (t.length) traits = t;
        }
      } else {
        warn("合成 LLM 応答を JSON 解析できず、機械フォールバックを使用");
      }
    } else {
      warn(`合成 LLM 失敗 (${res.error}) → 機械フォールバック`);
    }
  } catch (e) {
    warn(`合成 LLM 例外 (${(e as Error).message}) → 機械フォールバック`);
  }

  const persona: PoolPersona = {
    id: randomUUID(),
    name,
    role: "opinion",
    speechStyle,
    traits,
    affectVector,
    origin: "synthesized",
    parentIds: parents.map((p) => p.id),
    label: args.label ?? parents.map((p) => p.label ?? p.name).join("×"),
  };
  insertPoolPersona(persona);
  return persona;
}
