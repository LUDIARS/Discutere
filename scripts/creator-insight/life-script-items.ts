/**
 * life-script-items — 人生脚本アンケートを、チャットで 1 問ずつ出す順番に並べる。
 *
 * 入力: 設問 JSON のパス (省略時は skills/creator-insight/data/life-script-questionnaire.ja.json)
 * 出力 (stdout, JSON): { questionnaire, revision, instructions, prompt, total, items: [{ n, id, section, kind, text, choices }] }
 *   - kind=likert の choices は 1〜4 の評定ラベル。kind=forced の choices は a〜e の選択肢。
 *   - セクションの中の順番は設問 JSON の presentation.seed から決める (同じ設問 JSON なら毎回同じ順番)。
 *   - 尺度名 (key) と逆転の有無は出さない。答える人に見せる文言だけを出す。
 * 上限: 入力 256KB。ネットワークに出ない。LLM を呼ばない。
 *
 * 実行: npm run insight:life-script-items [-- <設問 JSON>]
 */
import path from "node:path";
import { fileURLToPath } from "node:url";
import { failCli, loadQuestionnaire, type Questionnaire } from "./lib/questionnaire.js";

const HERE = path.dirname(fileURLToPath(import.meta.url));
export const DEFAULT_QUESTIONNAIRE = path.resolve(HERE, "../../skills/creator-insight/data/life-script-questionnaire.ja.json");

export interface ChatItem {
  n: number;
  id: string;
  section: string;
  kind: "likert" | "forced";
  text: string;
  choices: Array<{ value: string; label: string }>;
}

export interface ChatItems {
  questionnaire: string;
  revision: string;
  instructions: string[];
  prompt: string;
  total: number;
  items: ChatItem[];
}

/** 文字列から 32bit の種を作る (FNV-1a)。 */
function hashSeed(text: string): number {
  let h = 0x811c9dc5;
  for (let i = 0; i < text.length; i += 1) {
    h ^= text.charCodeAt(i);
    h = Math.imul(h, 0x01000193) >>> 0;
  }
  return h;
}

/** 種つきの擬似乱数 (mulberry32)。 */
function prng(seed: number): () => number {
  let a = seed;
  return () => {
    a = (a + 0x6d2b79f5) >>> 0;
    let t = a;
    t = Math.imul(t ^ (t >>> 15), t | 1);
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

export function seededShuffle<T>(list: readonly T[], seed: string): T[] {
  const out = [...list];
  const rand = prng(hashSeed(seed));
  for (let i = out.length - 1; i > 0; i -= 1) {
    const j = Math.floor(rand() * (i + 1));
    [out[i], out[j]] = [out[j]!, out[i]!];
  }
  return out;
}

export function buildChatItems(q: Questionnaire): ChatItems {
  const likertChoices = ["1", "2", "3", "4"].map((v) => ({ value: v, label: q.scale.labels[v] ?? v }));
  const items: ChatItem[] = [];
  let prompt = "";
  for (const section of q.sections) {
    const shuffle = q.presentation.shuffleWithinSection;
    const seed = `${q.presentation.seed}/${section.id}`;
    if (section.type === "likert") {
      const ordered = shuffle ? seededShuffle(section.items, seed) : section.items;
      for (const item of ordered) {
        items.push({ n: items.length + 1, id: item.id, section: section.title, kind: "likert", text: item.text, choices: likertChoices });
      }
    } else {
      prompt = section.prompt;
      const ordered = shuffle ? seededShuffle(section.items, seed) : section.items;
      for (const item of ordered) {
        items.push({
          n: items.length + 1, id: item.id, section: section.title, kind: "forced", text: item.scene,
          choices: item.options.map((o) => ({ value: o.id, label: o.text })),
        });
      }
    }
  }
  return { questionnaire: q.id, revision: q.revision, instructions: q.instructions, prompt, total: items.length, items };
}

function main(): void {
  const file = process.argv[2] ?? DEFAULT_QUESTIONNAIRE;
  process.stdout.write(JSON.stringify(buildChatItems(loadQuestionnaire(file)), null, 2) + "\n");
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  try { main(); } catch (e) { failCli(e); }
}
