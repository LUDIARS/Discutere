/**
 * ペルソナ生成エンジン (C2-a)。
 *
 * アンケート回答 (SurveyProfile) を持つ合成個体を生成し、ランダム抽選ゲームについて
 * 「プレイしたか」を行動基準で確率判定 → プレイ済みのみ LLM で感想を生成 → affect 化 →
 * 個体の意見平均を affect ベクトルとしてプールに保存する (origin="generated", learningSource="survey")。
 * 未プレイは明示的に数えるだけ (意見にしない = 「やってないものはやってない」)。
 *
 * 母数判断 (案A) は persona-populations.ts (C2-b) が実クロール分布と突合して行う。
 */

import { randomUUID } from "node:crypto";
import type { LLMClient } from "../persona-engine/llm/client.js";
import { textToVector } from "./sentiment-vector.js";
import { averageVectors } from "./persona-synthesize.js";
import { insertPoolPersona, type PoolPersona } from "./persona-pool.js";
import {
  randomProfile,
  playProbability,
  profileSummary,
  profileToTraits,
  type SurveyProfile,
  type Genre,
} from "./survey.js";

/** 合成個体に提示するゲーム。genre があると play 判定が嗜好依存になる。 */
export interface SurveyGame {
  slug: string;
  title: string;
  genre?: Genre | string;
  /** メカニクス概要 (感想生成の条件付け。任意)。 */
  mechanics?: string;
}

export interface SurveyGenArgs {
  count: number;
  games: SurveyGame[];
  llm: LLMClient;
  rng?: () => number;
  /** 1 個体が検討するゲーム数 (既定 = min(games.length, 8))。 */
  gamesPerIndividual?: number;
  /** プロフィールを外部指定する (省略時 randomProfile)。 */
  profiles?: SurveyProfile[];
  warn?: (msg: string) => void;
}

export interface SurveyGenResult {
  personas: PoolPersona[];
  /** 個体ごとの内訳 (プレイ済/未プレイ件数)。 */
  breakdown: Array<{ id: string; played: number; unplayed: number }>;
}

function shuffle<T>(arr: T[], rng: () => number): T[] {
  const a = [...arr];
  for (let i = a.length - 1; i > 0; i--) {
    const j = Math.floor(rng() * (i + 1));
    [a[i], a[j]] = [a[j], a[i]];
  }
  return a;
}

function buildImpressionPrompt(profile: SurveyProfile, game: SurveyGame): string {
  return (
    `あなたは次のプロフィールのゲーマーです: ${profileSummary(profile)}\n` +
    `ゲーム「${game.title}」${game.mechanics ? ` (${game.mechanics})` : ""} を実際に遊んだ感想を、` +
    `あなたの嗜好・求める体験に正直に、自然な口語で 1〜2 文で述べてください。` +
    `良い点も不満点も率直に。発言テキストのみ返答 (前置き不要)。`
  );
}

/**
 * `count` 体の合成ペルソナを生成してプールに保存する。
 * プレイ判定 → 感想生成 (LLM) → affect 平均。1 件も感想が無い個体は保存しない。
 */
export async function generateSyntheticPersonas(args: SurveyGenArgs): Promise<SurveyGenResult> {
  const warn = args.warn ?? ((m) => console.warn(`[survey/warn] ${m}`));
  const rng = args.rng ?? Math.random;
  const perInd = args.gamesPerIndividual ?? Math.min(args.games.length, 8);

  const personas: PoolPersona[] = [];
  const breakdown: SurveyGenResult["breakdown"] = [];

  for (let i = 0; i < args.count; i++) {
    const profile = args.profiles?.[i] ?? randomProfile(rng);
    const games = shuffle(args.games, rng).slice(0, perInd);
    const vectors: number[][] = [];
    let played = 0;
    let unplayed = 0;

    for (const game of games) {
      if (rng() >= playProbability(profile, game.genre)) {
        unplayed++; // 「やってない」= 意見にしない
        continue;
      }
      try {
        const res = await args.llm.invoke({ prompt: buildImpressionPrompt(profile, game), maxTokens: 160 });
        if (res.ok && res.text.trim()) {
          vectors.push(textToVector(res.text.trim()));
          played++;
        } else {
          unplayed++;
          if (!res.ok) warn(`感想生成失敗 (${game.slug}): ${res.error}`);
        }
      } catch (e) {
        unplayed++;
        warn(`感想生成例外 (${game.slug}): ${(e as Error).message}`);
      }
    }

    if (vectors.length === 0) {
      breakdown.push({ id: "(skipped)", played, unplayed });
      continue; // 全部未プレイ → 個体として保存しない
    }
    const persona: PoolPersona = {
      id: randomUUID(),
      name: `合成#${randomUUID().slice(0, 6)}`,
      role: "opinion",
      speechStyle: "",
      traits: profileToTraits(profile),
      affectVector: averageVectors(vectors),
      origin: "generated",
      parentIds: [],
      learningSource: "survey",
      label: profileSummary(profile),
    };
    insertPoolPersona(persona);
    personas.push(persona);
    breakdown.push({ id: persona.id, played, unplayed });
  }

  return { personas, breakdown };
}
