/**
 * dialectic スケジューラ — 決定的コード (respec 06 §3, dialectic.md §3)。
 *
 * ファシリテーターの中身。優先度:
 *   ① 未応答 rebut への応答権 (被反論者を指名)
 *   ② 攻撃番 (接続数最大の Position を、反対陣営の発言数最少ペルソナが攻める)
 *   ③ 発言数最少ペルソナの自由ターン
 *
 * LLM は指名を自然な進行文にレンダリングするだけ (renderNomination、失敗は決定的
 * fallback 文で degrade)。同点はすべて安定順 (入力順 / id 昇順) で決定的。
 */

import type { LLMClient } from "../../persona-engine/llm/client.js";
import type { FlowPersona } from "../personas.js";
import type { ArgumentGraph, DialogueAct } from "../argument-graph.js";
import type { UtteranceDigest } from "./turn-prompt.js";

/** 応答権指名で許可する act (respec 05 §1: 防御/譲歩/再反論/確認)。 */
export const RESPOND_ACTS: readonly DialogueAct[] = ["support", "concede", "rebut", "question"];
/** 攻撃番で許可する act。 */
export const ATTACK_ACTS: readonly DialogueAct[] = ["rebut", "question"];
/** 自由ターンで許可する act (synthesize は [4] 専用のため除外)。 */
export const FREE_ACTS: readonly DialogueAct[] = ["claim", "support", "rebut", "concede", "question"];

/** issue 内の Position claim 発話 (攻撃番のターゲット候補)。 */
export interface PositionAnchor {
  personaId: string;
  stance: "pro" | "con";
  utteranceId: string;
}

export interface ScheduleArgs {
  graph: ArgumentGraph;
  /** 発言可能ペルソナ (facilitator 除外・入力順が同点タイブレークの安定順)。 */
  speakers: readonly FlowPersona[];
  /** 当該 issue の Position claim 発話。 */
  positionAnchors: readonly PositionAnchor[];
  /** セッション累計の発言数 (personaId → count)。 */
  speakCounts: ReadonlyMap<string, number>;
}

export interface ScheduleDecision {
  kind: "respond" | "attack" | "free";
  personaId: string;
  /** 応答/攻撃のターゲット utterance id (free は null)。 */
  targetUtteranceId: string | null;
  allowedActs: readonly DialogueAct[];
  /** 指名理由 (ファシリテーターレンダリングの入力 + ログ)。 */
  reason: string;
}

function minSpeakCountPersona(
  candidates: readonly FlowPersona[],
  speakCounts: ReadonlyMap<string, number>
): FlowPersona | null {
  let best: FlowPersona | null = null;
  let bestCount = Infinity;
  for (const p of candidates) {
    const c = speakCounts.get(p.id) ?? 0;
    if (c < bestCount) {
      best = p;
      bestCount = c;
    }
  }
  return best;
}

/**
 * 次の一手を決定的に決める。発言者がいなければ null。
 */
export function scheduleNextTurn(args: ScheduleArgs): ScheduleDecision | null {
  const { graph, speakers, positionAnchors, speakCounts } = args;
  if (speakers.length === 0) return null;
  const speakerIds = new Set(speakers.map((p) => p.id));

  // ① 未応答 rebut への応答権 (最古の未応答辺から。被反論者を指名)。
  for (const edge of graph.unresolvedRebuttals()) {
    if (edge.targetPersonaId && speakerIds.has(edge.targetPersonaId)) {
      return {
        kind: "respond",
        personaId: edge.targetPersonaId,
        targetUtteranceId: edge.rebutId,
        allowedActs: RESPOND_ACTS,
        reason: "未応答の反論に応答権を指名",
      };
    }
  }

  // ② 攻撃番: 接続数最大の Position (同点は anchor 入力順) を反対陣営の発言数最少が攻める。
  let focal: PositionAnchor | null = null;
  let focalEdges = -1;
  for (const anchor of positionAnchors) {
    const n = graph.edgeCountFor(anchor.utteranceId);
    if (n > focalEdges) {
      focal = anchor;
      focalEdges = n;
    }
  }
  if (focal) {
    const opposing = speakers.filter(
      (p) => p.role === "debater" && p.stance !== focal!.stance && p.id !== focal!.personaId
    );
    const attacker = minSpeakCountPersona(opposing, speakCounts);
    if (attacker) {
      return {
        kind: "attack",
        personaId: attacker.id,
        targetUtteranceId: focal.utteranceId,
        allowedActs: ATTACK_ACTS,
        reason: "対立の焦点 (接続数最大の Position) への攻撃番を指名",
      };
    }
  }

  // ③ 発言数最少ペルソナの自由ターン (同点は speakers 入力順)。
  const free = minSpeakCountPersona(speakers, speakCounts);
  if (!free) return null;
  return {
    kind: "free",
    personaId: free.id,
    targetUtteranceId: null,
    allowedActs: FREE_ACTS,
    reason: "発言数最少ペルソナの自由ターン",
  };
}

// ── ファシリテーターの露出文レンダリング ─────────────────────────────────────

/** 決定的 fallback 文 (LLM 失敗時 / テスト時)。 */
export function nominationFallbackText(
  decision: ScheduleDecision,
  personaName: string,
  target?: UtteranceDigest
): string {
  switch (decision.kind) {
    case "respond":
      return target
        ? `${target.personaName}さんの「${target.text.slice(0, 60)}」にまだ応答がありません。${personaName}さん、どう応えますか?`
        : `${personaName}さん、先ほどの反論にどう応えますか?`;
    case "attack":
      return target
        ? `${personaName}さん、${target.personaName}さんの主張「${target.text.slice(0, 60)}」について、反対の立場から意見をどうぞ。`
        : `${personaName}さん、反対の立場から意見をどうぞ。`;
    case "free":
      return `${personaName}さん、この論点について意見をどうぞ。`;
  }
}

export interface RenderFacilitatorLineArgs {
  /** レンダリング対象の進行内容 (決定的 fallback 文を兼ねる)。 */
  content: string;
  /** withCostLog 済み進行役 LLM (roster.facilitator, location="facilitator")。 */
  llm: LLMClient;
  model?: string;
  warn?: (msg: string) => void;
}

/**
 * 進行内容を自然な進行文にレンダリングする (LLM はレンダリングのみ)。
 * LLM 失敗 / 空応答は content そのままへ degrade (進行は止めない)。
 */
export async function renderFacilitatorLine(args: RenderFacilitatorLineArgs): Promise<string> {
  const { content, llm, model, warn = () => {} } = args;
  const result = await llm.invoke({
    prompt:
      `あなたは議論の進行役です。次の進行内容を、自然な口語 1〜2 文にしてください` +
      `(意味を変えない・装飾やラベルを付けない):\n` +
      `内容: ${content}`,
    ...(model ? { model } : {}),
    maxTokens: 300,
  });
  if (!result.ok) {
    warn(`ファシリテーター進行文レンダリング失敗: ${result.error} — 定型文で続行 (degrade)`);
    return content;
  }
  const text = result.text.trim();
  return text || content;
}

export interface RenderNominationArgs {
  decision: ScheduleDecision;
  personaName: string;
  target?: UtteranceDigest;
  /** withCostLog 済み進行役 LLM (roster.facilitator, location="facilitator")。 */
  llm: LLMClient;
  model?: string;
  warn?: (msg: string) => void;
}

/**
 * 指名を自然な進行文にレンダリングする。LLM 失敗は決定的 fallback 文へ degrade。
 */
export async function renderNomination(args: RenderNominationArgs): Promise<string> {
  const { decision, personaName, target, ...rest } = args;
  return renderFacilitatorLine({ ...rest, content: nominationFallbackText(decision, personaName, target) });
}
