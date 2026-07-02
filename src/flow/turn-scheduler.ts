/**
 * ターンスケジューラ — シャッフル輪番 (respec 02, A2)。
 *
 * 毎ターンのランダム抽選 (同一人物の連続発言・1 ラウンド無発言・pro 不在ラウンドが
 * 確率で起きる) を廃止し、ラウンド開始時に facilitator を除くペルソナ列をシャッフルして
 * その順に巡回する。turnsPerRound ≥ 人数 なら全員が最低 1 回発言することを保証する。
 *
 * 純関数 (Rng 注入で決定的)。PR-B の指名制ディレクターはこの上位互換として導入予定。
 */

import type { FlowPersona, Rng } from "./personas.js";

/** Fisher–Yates シャッフル (非破壊・rng 注入で決定的)。 */
export function shuffle<T>(items: readonly T[], rng: Rng): T[] {
  const arr = [...items];
  for (let i = arr.length - 1; i > 0; i--) {
    const j = Math.floor(rng() * (i + 1));
    [arr[i], arr[j]] = [arr[j], arr[i]];
  }
  return arr;
}

/**
 * ラウンドの発言順を組み立てる (シャッフル輪番)。
 *
 * - facilitator は開幕ターン (turn 0) 専任のため対象から除外する。
 * - turnsPerRound > 人数 のときは周回ごとに再シャッフルする。周回境界で
 *   前周の末尾と次周の先頭が同一なら swap し、連続同一発言者を出さない。
 * - turnsPerRound < 人数 でもシャッフル順の先頭から重複なく充てる。
 *
 * @returns 長さ turnsPerRound の発言者列 (発言者が 0 人なら空配列)。
 */
export function buildTurnOrder(
  personas: readonly FlowPersona[],
  turnsPerRound: number,
  rng: Rng
): FlowPersona[] {
  const speakers = personas.filter((p) => p.role !== "facilitator");
  if (speakers.length === 0 || turnsPerRound <= 0) return [];

  const order: FlowPersona[] = [];
  while (order.length < turnsPerRound) {
    const lap = shuffle(speakers, rng);
    const prev = order[order.length - 1];
    // 周回境界の連続同一発言者を避ける (前周末尾 = 次周先頭なら 2 番手と swap)。
    if (prev && lap.length > 1 && lap[0].id === prev.id) {
      [lap[0], lap[1]] = [lap[1], lap[0]];
    }
    order.push(...lap);
  }
  return order.slice(0, turnsPerRound);
}
