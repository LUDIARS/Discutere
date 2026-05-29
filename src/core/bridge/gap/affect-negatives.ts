// 負 Affect 集合。source of truth は data/affects/vocabulary.json の valence === "negative"。
// (matcher を pure に保つため runtime で json を読まず生成値を埋め込む。
//  語彙更新時は `npm run seed:affects -- --emit-negatives` で再生成する想定。)
// 旧ハードコード ["frustration","anger","sadness","boredom"] の上位集合なので既存テストは不変。
export const NEGATIVE_AFFECTS: ReadonlySet<string> = new Set([
  "frustration",
  "anger",
  "sadness",
  "boredom",
  "monotony",
  "confusion",
  "unfair_monetization",
]);
