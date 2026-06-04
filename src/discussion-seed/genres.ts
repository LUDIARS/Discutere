/**
 * ジャンル別「何が面白い?」自動シード議論の種カタログ (#64)。
 *
 * 本来は ars-game-lexicon の 17 ジャンルを正本にしたいが、 cross-repo path 依存は
 * CI で壊れるため (LUDIARS 共通の教訓)、 Di 内に静的リストとして持つ。 lexicon 側の
 * 更新に追従したい場合は config or 引数でリストを差し替えられる設計にしてある。
 */

export interface GenreSeed {
  /** ジャンル名 (日本語) */
  genre: string;
  /** 議論の切り口を補足する一言 (任意) */
  angle?: string;
}

/** ars-game-lexicon 準拠の 17 ジャンル (静的フォールバック)。 */
export const GENRE_SEEDS: readonly GenreSeed[] = [
  { genre: "アクション", angle: "操作の手応えと上達の実感" },
  { genre: "シューティング", angle: "弾幕と回避のリスク・リワード" },
  { genre: "RPG", angle: "成長と物語への没入" },
  { genre: "シミュレーション", angle: "システムを最適化する快感" },
  { genre: "ストラテジー", angle: "先読みと意思決定の重み" },
  { genre: "アドベンチャー", angle: "探索と発見の驚き" },
  { genre: "パズル", angle: "「分かった!」の瞬間の設計" },
  { genre: "ローグライク", angle: "ランダム性と一回性のスリル" },
  { genre: "サバイバル", angle: "リソース管理と緊張の持続" },
  { genre: "対戦格闘", angle: "読み合いと習熟の深さ" },
  { genre: "スポーツ", angle: "再現性と一瞬の判断" },
  { genre: "レース", angle: "速度感とライン取りの妙" },
  { genre: "リズム", angle: "音楽との一体感とフロー" },
  { genre: "クラフト・サンドボックス", angle: "自由な創造と所有の喜び" },
  { genre: "ホラー", angle: "恐怖の演出と緊張の緩急" },
  { genre: "ノベル・アドベンチャー", angle: "選択と物語の重み" },
  { genre: "デッキ構築", angle: "シナジーを組み上げる戦略性" },
] as const;

export interface SeedTopic {
  title: string;
  description: string;
}

/** ジャンル種 → 「何が面白い?」議題テキストに展開する。 */
export function buildGenreTopic(seed: GenreSeed): SeedTopic {
  const angle = seed.angle ? `特に「${seed.angle}」の観点から、` : "";
  return {
    title: `【${seed.genre}】何が面白い?`,
    description:
      `「${seed.genre}」というジャンルの面白さの核は何か。${angle}` +
      `プレイヤーが夢中になる構造的な理由を、 複数の視点から掘り下げて議論する。`,
  };
}

/**
 * index でジャンルを 1 つ選ぶ (決定的)。 ランダム性は呼び出し側が index を振って表現する
 * (workflow / scheduler が Math.random を持てない制約に合わせ、 ここは純関数に保つ)。
 */
export function pickGenreSeed(index: number, seeds: readonly GenreSeed[] = GENRE_SEEDS): GenreSeed {
  if (seeds.length === 0) throw new Error("genre seed list is empty");
  return seeds[((index % seeds.length) + seeds.length) % seeds.length];
}
