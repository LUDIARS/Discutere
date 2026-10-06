/**
 * 議論の前提ルール (dialectic.md §2.5) — 型 + 正規化 + プロンプトへ載せるルール本文。
 *
 * AI 同士の議論は、次の 3 点について共通見解は取れている前提で進める (すり合わせは人間に必要な
 * 手順であり、AI の議論のゴールには置かない。止揚もゴールにしない):
 *   - どんな体験をつくりたいか
 *   - どんな施策 (アイデア) を試したいか
 *   - 他のゲームではどうしているか / どうなっているか
 * 論点のゴールは、論破 (一方の意見が出なくなる) / 合意 (ジンテーゼ = 結論の同一性) / 止揚 のいずれか。
 *
 * 論拠は 3 種類 (InferenceKind) に限り、各根拠はトゥールミンの
 * データ (data) と論拠 (warrant) を分けて持つ。主張には限定 (qualifier = 範囲) を付ける。
 * 反論は「データ / 論拠 / 限定」のどれを突くか (AttackPoint) を明示させる。
 *
 * LLM なし・判断なし。違反の検査は rule-check.ts が持つ (SRP)。
 */

/** 論拠の種類。 */
export type InferenceKind = "deduction" | "induction" | "abduction";

/** 反論が突くトゥールミン要素。 */
export type AttackPoint = "data" | "warrant" | "qualifier";

const INFERENCE_KINDS: readonly InferenceKind[] = ["deduction", "induction", "abduction"];
const ATTACK_POINTS: readonly AttackPoint[] = ["data", "warrant", "qualifier"];

export const INFERENCE_KIND_LABEL: Record<InferenceKind, string> = {
  deduction: "演繹",
  induction: "帰納",
  abduction: "アブダクション",
};

export const ATTACK_POINT_LABEL: Record<AttackPoint, string> = {
  data: "データ",
  warrant: "論拠",
  qualifier: "限定",
};

/** LLM 由来の文字列を InferenceKind に正規化する (未知値は null)。日本語ラベルも受ける。 */
export function coerceInferenceKind(v: unknown): InferenceKind | null {
  if (typeof v !== "string") return null;
  const s = v.trim().toLowerCase();
  if ((INFERENCE_KINDS as readonly string[]).includes(s)) return s as InferenceKind;
  for (const k of INFERENCE_KINDS) if (s === INFERENCE_KIND_LABEL[k].toLowerCase()) return k;
  return null;
}

/** LLM 由来の文字列を AttackPoint に正規化する (未知値は null)。日本語ラベルも受ける。 */
export function coerceAttackPoint(v: unknown): AttackPoint | null {
  if (typeof v !== "string") return null;
  const s = v.trim().toLowerCase();
  if ((ATTACK_POINTS as readonly string[]).includes(s)) return s as AttackPoint;
  for (const k of ATTACK_POINTS) if (s === ATTACK_POINT_LABEL[k]) return k;
  return null;
}

/** 全発話プロンプト (定立・反定立) に載せる前提ルール本文。 */
export const PREMISE_RULES_TEXT = [
  "# 議論の前提ルール (厳守)",
  "次の 3 点について共通見解は取れている前提で議論する (すり合わせ直さない):",
  "- どんな体験をつくりたいか",
  "- どんな施策 (アイデア) を試したいか",
  "- 他のゲームではどうしているか、どうなっているか",
  "論点のゴールは次のいずれかの状態: 合意 (両者が同じ結論に至る) / 止揚 (両者の根拠を保ったまま一段上の結論に至る) / 論破 (一方の根拠がすべて崩れ、意見が出なくなる)。",
  "",
  "論拠は次の 3 種類のどれかで立てる:",
  "- 演繹 (deduction): この仕様を入れて、こういう人が遊ぶと、こういう体験になる",
  "- 帰納 (induction): この仕様を入れている他のゲームでは、こういう体験になっていた (同じ仕様であることを示す)",
  "- アブダクション (abduction): こういう体験を作りたいから、きっとこういう仕様にしたら良い",
  "",
  "トゥールミンの整理:",
  "- 主張とデータは別物。データ (事実・事例) と論拠 (データから主張へ橋渡しする理由) を混ぜない",
  "- データが正しくても論拠が間違っていれば主張は間違いになる。反論はデータ・論拠・限定のどれを突くか決めて行う",
  "- 主張の限定 (どのプレイヤー層・どの条件の話か) をはっきりさせる",
  "",
  "禁止 (前提の外から相手の主張を曲げる手):",
  "- 相手の限定の外側にある層・条件・事例を持ち出して主張全体を否定する",
  "- 相手が言っていない主張に置き換えて叩く",
  "- 根拠の中身に触れず「それって感想」「データあるの?」だけで退ける (足りないなら何が要るかを具体的に問う)",
  "- 論点をずらす、別の論点にすり替える",
  "- 何を論拠として認めるかという前提を途中で変える",
].join("\n");
