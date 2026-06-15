/**
 * ペルソナ生成エンジン (C2) のアンケート行動基準スキーマ。
 *
 * 合成個体 = 1 組のアンケート回答 (SurveyProfile)。嗜好 / プレイスタイル / 感情の波 /
 * 年代 / 課金額 を構造化する。これを基に「ゲームをプレイしたか」を確率判定し、
 * プレイ済みゲームの感想を LLM で生成する (persona-survey.ts)。
 */

export const GENRES = [
  "roguelike",
  "gacha",
  "fps",
  "rpg",
  "puzzle",
  "action",
  "strategy",
  "horror",
  "simulation",
  "fighting",
] as const;
export type Genre = (typeof GENRES)[number];

export const PLAY_STYLES = ["casual", "hardcore", "completionist", "social", "competitive"] as const;
export type PlayStyle = (typeof PLAY_STYLES)[number];

/** 感情の波: 求める体験 (LLM 感想生成の条件付けに使う)。 */
export const EMOTION_SEEKS = ["興奮", "緊張", "達成感", "癒し", "没入", "交流", "収集", "成長"] as const;
export type EmotionSeek = (typeof EMOTION_SEEKS)[number];

export const AGE_BANDS = ["10s", "20s", "30s", "40s+"] as const;
export type AgeBand = (typeof AGE_BANDS)[number];

/** 課金額。none=無課金 / light=微 / medium=中 / whale=重課金。 */
export const SPENDING = ["none", "light", "medium", "whale"] as const;
export type Spending = (typeof SPENDING)[number];

export interface SurveyProfile {
  genres: Genre[];
  playStyles: PlayStyle[];
  emotionSeeks: EmotionSeek[];
  ageBand: AgeBand;
  spending: Spending;
}

type Rng = () => number;

function pick<T>(arr: readonly T[], rng: Rng): T {
  return arr[Math.floor(rng() * arr.length)] ?? arr[0];
}
/** arr から重複なく n..m 個選ぶ。 */
function pickSome<T>(arr: readonly T[], min: number, max: number, rng: Rng): T[] {
  const n = min + Math.floor(rng() * (max - min + 1));
  const pool = [...arr];
  const out: T[] = [];
  for (let i = 0; i < n && pool.length; i++) {
    out.push(pool.splice(Math.floor(rng() * pool.length), 1)[0]);
  }
  return out;
}

/** ランダムなアンケート回答を 1 件生成する。 */
export function randomProfile(rng: Rng): SurveyProfile {
  return {
    genres: pickSome(GENRES, 1, 3, rng),
    playStyles: pickSome(PLAY_STYLES, 1, 2, rng),
    emotionSeeks: pickSome(EMOTION_SEEKS, 1, 3, rng),
    ageBand: pick(AGE_BANDS, rng),
    spending: pick(SPENDING, rng),
  };
}

const SPENDING_FACTOR: Record<Spending, number> = { none: 0.0, light: 0.05, medium: 0.1, whale: 0.18 };

/**
 * プロフィールが当該ゲーム (genre) をプレイしている確率 (0..0.95)。
 * ジャンル一致で大きく上がり、課金傾向で微増。genre 不明なら中間。
 */
export function playProbability(profile: SurveyProfile, gameGenre?: Genre | string): number {
  const base = 0.12;
  const genreMatch = gameGenre && profile.genres.includes(gameGenre as Genre) ? 0.6 : gameGenre ? 0.05 : 0.25;
  const p = base + genreMatch + SPENDING_FACTOR[profile.spending];
  return Math.max(0, Math.min(0.95, p));
}

/** プロフィールを人間可読な短い説明にする (persona の label/prompt 用)。 */
export function profileSummary(p: SurveyProfile): string {
  return (
    `嗜好=${p.genres.join("/")} / スタイル=${p.playStyles.join("/")} / ` +
    `求める体験=${p.emotionSeeks.join("/")} / ${p.ageBand} / 課金=${p.spending}`
  );
}

/** プロフィールを persona traits 配列にする。 */
export function profileToTraits(p: SurveyProfile): string[] {
  return [...p.genres, ...p.playStyles, ...p.emotionSeeks, p.ageBand, `課金:${p.spending}`];
}
