/**
 * ユーザーの声の均等混合 (spec/feature/flow/user-voices.md)。
 *
 * 議題のゲームと類似ゲームの声を、ゲームごとに同じ件数ずつ交互に取り出して混ぜる
 * (どれか 1 本の声に議論が引っ張られないように)。足りないゲームの分は他で埋める。
 * 同じ本文は 1 回だけ使う。
 */

import type { ContextVoice } from "../discussion-paper.js";

/** グループを先頭から 1 件ずつ交互に取り、limit 件まで混ぜる (純関数)。 */
export function mixEvenly<T>(groups: readonly (readonly T[])[], limit: number, keyOf: (v: T) => string): T[] {
  const out: T[] = [];
  const seen = new Set<string>();
  const cursors = groups.map(() => 0);
  let progressed = true;
  while (out.length < limit && progressed) {
    progressed = false;
    for (let g = 0; g < groups.length && out.length < limit; g += 1) {
      const group = groups[g];
      while (cursors[g] < group.length) {
        const item = group[cursors[g]];
        cursors[g] += 1;
        const key = keyOf(item);
        if (seen.has(key)) continue;
        seen.add(key);
        out.push(item);
        progressed = true;
        break;
      }
    }
  }
  return out;
}

/** 声 1 件の重複判定キー (前後空白を除いた本文)。 */
export function voiceKey(v: ContextVoice): string {
  return v.content.trim();
}

/**
 * 議論フローに渡す listExternalVoices を、ゲームごとの声の均等混合にする。
 * @param fetchGroups (terms, perGroupLimit) → ゲームごとの声の配列 (先頭が議題のゲーム)。
 */
export function makeMixedVoiceLookup(
  fetchGroups: (terms: string[], perGroupLimit: number) => ContextVoice[][]
): (terms: string[], limit: number) => ContextVoice[] {
  return (terms, limit) => {
    if (limit <= 0) return [];
    // 均等に取るために、各ゲームから limit 件ずつ候補を引いてから交互に混ぜる。
    const groups = fetchGroups(terms, limit);
    return mixEvenly(groups, limit, voiceKey);
  };
}
