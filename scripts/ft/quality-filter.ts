/**
 * FT 教師データの決定的品質フィルタ (spec/feature/gemma-ft.md §9-1)。
 *
 * teacher (Claude) の全出力を無選別に学習させると、失敗ターン (浅い相槌・
 * 論点の繰り返し・system 指示の復唱) まで student がコピーする。ここは
 * LLM を使わない機械判定だけで除外する層 — 判定理由を必ず返し、呼び出し側が
 * 理由別に集計ログを出す (silent drop にしない)。
 *
 * 純関数 + 明示 state (createQualityFilter が閉じ込める) で単体テスト可能。
 * DB / ファイルに触れない。
 */

import { bigramJaccardSimilarity } from "../../src/core/vectors/hybrid-rank.js";

export interface QualitySampleInput {
  reqId: string;
  /** persona (worker)。繰り返し判定と採用上限はこの単位で見る。 */
  workerId: string;
  /** system prompt (役割リーク判定の照合元)。 */
  system: string;
  /** 学習対象の assistant 発話 (trim 済みを渡す)。 */
  assistant: string;
  /**
   * セッション識別子 (あれば)。現行の turn writer は未出力だが、将来 worker-pool が
   * 持たせたら groupCap がこの単位に切り替わる (無い間は workerId で代用)。
   */
  sessionId?: string;
}

export type RejectReason = "too_short" | "repetition" | "role_leak" | "group_cap";

export interface QualityVerdict {
  accepted: boolean;
  reason?: RejectReason;
}

export interface QualityFilterOptions {
  /** これ未満の本文は浅い相槌とみなして落とす (文字数)。 */
  minLength?: number;
  /** 同一 worker の直前採用ターンとの bigram Jaccard がこの値超なら繰り返し。 */
  repetitionThreshold?: number;
  /** system の 1 行 (この文字数以上) が本文にそのまま現れたら役割リーク。 */
  roleLeakMinLineLength?: number;
  /** 同一グループ (sessionId、無ければ workerId) からの採用上限。0 = 無制限。 */
  groupCap?: number;
}

const DEFAULTS: Required<QualityFilterOptions> = {
  minLength: 40,
  repetitionThreshold: 0.9,
  roleLeakMinLineLength: 30,
  groupCap: 0,
};

/** system prompt から役割リーク照合用の「長い行」を取り出す。 */
function leakCandidateLines(system: string, minLineLength: number): string[] {
  return system
    .split(/\r?\n/)
    .map((l) => l.trim())
    .filter((l) => l.length >= minLineLength);
}

/**
 * フィルタ判定器を作る。状態 (worker ごとの直前採用テキスト・グループ採用数) を
 * 内部に持つため、1 エクスポート実行につき 1 つ作って使い捨てる。
 * @implements SPEC-GEMMA-FT-DISTILL-QUALITY
 */
export function createQualityFilter(
  options: QualityFilterOptions = {}
): (sample: QualitySampleInput) => QualityVerdict {
  const opts = { ...DEFAULTS, ...options };
  /** worker → 直前に採用した本文 (繰り返し判定は「直前の採用」とだけ比較する)。 */
  const lastAcceptedByWorker = new Map<string, string>();
  /** グループ (sessionId 優先、無ければ workerId) → 採用数。 */
  const acceptedByGroup = new Map<string, number>();
  /** system 文字列 → リーク照合行 (system は persona ごとにほぼ固定なので memoize)。 */
  const leakLinesBySystem = new Map<string, string[]>();

  return (sample: QualitySampleInput): QualityVerdict => {
    const text = sample.assistant.trim();
    if (text.length < opts.minLength) {
      return { accepted: false, reason: "too_short" };
    }

    let leakLines = leakLinesBySystem.get(sample.system);
    if (!leakLines) {
      leakLines = leakCandidateLines(sample.system, opts.roleLeakMinLineLength);
      leakLinesBySystem.set(sample.system, leakLines);
    }
    if (leakLines.some((line) => text.includes(line))) {
      return { accepted: false, reason: "role_leak" };
    }

    const previous = lastAcceptedByWorker.get(sample.workerId);
    if (
      previous !== undefined &&
      bigramJaccardSimilarity(previous, text) > opts.repetitionThreshold
    ) {
      return { accepted: false, reason: "repetition" };
    }

    const groupKey = sample.sessionId ?? sample.workerId;
    const groupCount = acceptedByGroup.get(groupKey) ?? 0;
    if (opts.groupCap > 0 && groupCount >= opts.groupCap) {
      return { accepted: false, reason: "group_cap" };
    }

    lastAcceptedByWorker.set(sample.workerId, text);
    acceptedByGroup.set(groupKey, groupCount + 1);
    return { accepted: true };
  };
}
