/**
 * Facilitator — 議論の活性化と収束を司る per-discussion オーケストレータ。
 * spec/facilitator/DESIGN.md。
 *
 * 各 discussion-of-gap session を独自 tick で見張り:
 *  - 発言間隔が空いた (idle) → 新しい視点の persona を 1 体生成して投入し議論を広げる
 *  - 参加 persona が maxPersonas を超えた → 議論を要約して収束、 gap を closed に
 * gap が closed になるまで存在する (= 収束したら当該 session の管理を終える)。
 *
 * 投稿は contextProvider.postUtterance を通すので、 既存の Discord relay に乗る。
 */

import { randomUUID } from "node:crypto";

import { ensureReactionTables, getOpinionScore } from "../../discord-hook/reactions.js";
import {
  ensureFacilitatorDirectiveTable,
  formatDirectivesBlock,
  listFacilitatorDirectives,
} from "../../discord-hook/facilitator-directives.js";
import { ensureExclusionTable } from "../../core/noise/exclusions.js";
import type { createCore } from "../../core/index.js";
import type { PersonasRepo } from "../db/personas-repo.js";
import type { DiscussionContextProvider } from "../context-provider.js";
import type { LLMClient } from "../llm/client.js";
import { resolveTierModel, type TierModels } from "../llm/tier-routing.js";
import type { Logger } from "../types.js";
import type { FacilitatorTuning } from "../../runtime-settings/store.js";

import {
  buildAufhebungJudgePrompt,
  buildConvergePrompt,
  buildExpandPrompt,
  extractJsonObject,
  type GeneratedPersona,
  type RecentUtterance,
} from "./prompts.js";

type Core = ReturnType<typeof createCore>;

export interface FacilitatorOptions {
  /** 見張り周期 ms (既定 30_000) */
  tickMs?: number;
  /** 発言が無くなってから「停滞」とみなす空白 ms (既定 120_000) */
  idleGapMs?: number;
  /** 参加 persona がこの数を超えたら強制収束 (安全上限、 既定 20) */
  maxPersonas?: number;
  /** 止揚 (アウフヘーベン) がこの数たまったら収束 (既定 3) */
  aufhebungTarget?: number;
  /** LLM model 上書き (tierModels 未設定時の既定、 または cheap/strong の最終 fallback) */
  model?: string;
  /**
   * tier 別モデル (任意)。設定すると facilitator の重いタスク (収束/止揚/視点追加) を
   * strong に振り分ける。未設定なら従来通り `model` (または backend 既定) を使う。
   */
  tierModels?: TierModels;
}

/** 議論収束イベント (gap closed の直後に発火)。discord 依存は持たせない純データ。 */
export interface FacilitatorConvergedEvent {
  gapId: string;
  sessionId: string;
  /** 議論 session の scene ("discord:<guild>/<channel>" or "gap:<gapId>")。 */
  scene: string | null;
  /** 議題タイトル。 */
  title: string;
  /** 収束まとめ本文 (【収束】プレフィックス無し)。 */
  summary: string;
}

export interface FacilitatorDeps {
  core: Core;
  llm: LLMClient;
  contextProvider: DiscussionContextProvider;
  personas: PersonasRepo;
  workspaceId: string;
  logger: Logger;
  options?: FacilitatorOptions;
  /**
   * 収束トリガーのライブ設定取得 (任意)。 渡すと tick ごとにここから
   * maxPersonas / aufhebungTarget / idleGapMs / convergePolicy を読み、 Web UI からの
   * 変更を再起動なしで反映する。 未指定なら options (boot 時固定) を使う。
   */
  tuning?: () => FacilitatorTuning;
  /**
   * 議論が収束し gap を closed にした直後に呼ばれる (任意)。
   * フォーラム集約ではこのフックでスレッドを archive+lock し、まとめを転記する。
   * 失敗しても収束処理は止めない (fire-and-forget)。
   */
  onConverged?: (event: FacilitatorConvergedEvent) => void;
}

interface SessionState {
  lastUtteranceCount: number;
  armed: boolean; // 次の idle で介入してよいか (発言があると再 arm)
}

interface SessionMetrics {
  utteranceCount: number;
  distinctPersonas: number;
  lastActivityAt: number;
  now: number;
}

export type FacilitatorMode = "active" | "wait" | "intervene";

/**
 * 介入タイミング判定 (純粋関数、 テスト可能)。
 * - 新しい発言があった → active (再 arm)
 * - 発言間隔が空いた (idle) かつ armed → intervene (止揚判定 + 拡張/収束は呼び出し側)
 */
export function evaluate(
  metrics: SessionMetrics,
  state: SessionState,
  opts: { idleGapMs: number }
): { mode: FacilitatorMode; state: SessionState } {
  if (metrics.utteranceCount > state.lastUtteranceCount) {
    return { mode: "active", state: { lastUtteranceCount: metrics.utteranceCount, armed: true } };
  }
  const idleFor = metrics.now - metrics.lastActivityAt;
  if (state.armed && idleFor >= opts.idleGapMs) {
    return { mode: "intervene", state: { ...state, armed: false } };
  }
  return { mode: "wait", state };
}

export const FACILITATOR_PERSONA_ID = "facilitator";

/**
 * 進行役 persona のプロフィール (収束のまとめ発言者 / headless seed の開幕も担う)。
 * seed 側でも同じ persona を登録・登場させるため共有する。
 */
export const FACILITATOR_PERSONA = {
  id: FACILITATOR_PERSONA_ID,
  name: "進行役",
  display_name: "司会 結",
  description: "議論の活性化と収束を司る中立の進行役。",
  traits: ["中立", "俯瞰", "要約", "合意形成"],
  speech_style:
    "落ち着いた中立的な口調。 論点を整理し、 停滞時は新しい問いを投げ、 出揃ったら要約して締める。",
} as const;

export interface Facilitator {
  start(): void;
  stop(): void;
  /** 1 周分を手動実行 (テスト / デバッグ) */
  tickOnce(): Promise<void>;
  /**
   * 指定議論を即時に強制収束する (管制 UI の「まとめて閉じる」)。
   * auto-tick (start) が未起動でも動く (内部 converge は自己完結)。
   * 既に closed/converged/dismissed の gap は何もしない。
   */
  convergeNow(input: { sessionId: string; gapId: string }): Promise<{ ok: boolean; error?: string }>;
}

export function createFacilitator(deps: FacilitatorDeps): Facilitator {
  // boot 時 options を既定 tuning とし、 deps.tuning があれば tick ごとに
  // ライブ値 (Web UI からの変更) を読む。 tickMs だけは setInterval に焼くため固定。
  const defaultTuning: FacilitatorTuning = {
    tickMs: deps.options?.tickMs ?? 30_000,
    idleGapMs: deps.options?.idleGapMs ?? 120_000,
    maxPersonas: deps.options?.maxPersonas ?? 20,
    aufhebungTarget: deps.options?.aufhebungTarget ?? 3,
    convergePolicy: "default",
  };
  const getTuning = (): FacilitatorTuning => deps.tuning?.() ?? defaultTuning;
  const tickMs = defaultTuning.tickMs;
  const raw = deps.core.client.raw;
  const states = new Map<string, SessionState>();
  let timer: NodeJS.Timeout | null = null;
  let running = false;

  // スコア参照のため reaction テーブルを冪等確保 + 止揚ストックテーブル。
  ensureReactionTables(raw);
  raw.exec(
    `CREATE TABLE IF NOT EXISTS aufhebung_stock (
       id TEXT PRIMARY KEY,
       gap_id TEXT NOT NULL,
       summary TEXT NOT NULL,
       created_at INTEGER NOT NULL
     )`
  );
  // ノイズ除外サイドカー表を冪等確保 (まとめ生成時に除外発話を弾くため)。
  ensureExclusionTable(raw);
  // 進行役への調整指示サイドカー表を冪等確保 (gapTopic で読み、 expand/converge に効かせる)。
  ensureFacilitatorDirectiveTable(raw);

  // 進行役 persona を登録 (収束のまとめ発言者)
  deps.personas.insertOrIgnore({ ...FACILITATOR_PERSONA, traits: [...FACILITATOR_PERSONA.traits] });

  interface Discussion {
    sessionId: string;
    gapId: string;
  }

  /**
   * open な議論 (discussion-of-gap session) を列挙。
   * Discord 紐付け (scene=discord:*) に加え、 headless 議論 (scene=gap:*、 自動シード等) も
   * 駆動対象に含める。 headless は postUtterance が Discord に出ないだけで、 拡張/収束の
   * オーケストレーション自体は同じ機構で回す (#63-65 の土台)。
   */
  function listActiveDiscussions(): Discussion[] {
    const rows = raw
      .prepare(
        `SELECT s.id AS sessionId, s.title AS title
           FROM sessions s
          WHERE s.workspace_id = ?
            AND s.title LIKE 'discussion-of-gap:%'
            AND (s.scene LIKE 'discord:%' OR s.scene LIKE 'gap:%')`
      )
      .all(deps.workspaceId) as Array<{ sessionId: string; title: string }>;
    const out: Discussion[] = [];
    for (const r of rows) {
      const gapId = r.title.slice("discussion-of-gap:".length);
      const gap = raw
        .prepare("SELECT status FROM design_gaps WHERE id = ?")
        .get(gapId) as { status: string | null } | undefined;
      if (!gap) continue;
      if (gap.status && ["closed", "converged", "dismissed"].includes(gap.status)) continue;
      out.push({ sessionId: r.sessionId, gapId });
    }
    return out;
  }

  function metricsFor(sessionId: string, now: number): SessionMetrics {
    const rows = raw
      .prepare(
        "SELECT speaker_id, posted_at FROM utterances WHERE session_id = ? AND id NOT IN (SELECT utterance_id FROM utterance_exclusions)"
      )
      .all(sessionId) as Array<{ speaker_id: string | null; posted_at: number }>;
    const personaIds = new Set<string>();
    let lastActivityAt = 0;
    for (const u of rows) {
      if (u.speaker_id?.startsWith("persona:")) personaIds.add(u.speaker_id);
      if (u.posted_at > lastActivityAt) lastActivityAt = u.posted_at;
    }
    return { utteranceCount: rows.length, distinctPersonas: personaIds.size, lastActivityAt, now };
  }

  function recentUtterances(sessionId: string, limit: number): RecentUtterance[] {
    const rows = raw
      .prepare(
        "SELECT speaker_id, raw_content FROM utterances WHERE session_id = ? AND id NOT IN (SELECT utterance_id FROM utterance_exclusions) ORDER BY posted_at DESC LIMIT ?"
      )
      .all(sessionId, limit) as Array<{ speaker_id: string | null; raw_content: string }>;
    return rows
      .reverse()
      .map((u) => ({ speaker: speakerLabel(u.speaker_id), content: u.raw_content }));
  }

  function speakerLabel(speakerId: string | null): string {
    if (!speakerId) return "参加者";
    if (speakerId.startsWith("persona:")) {
      const p = deps.personas.get(speakerId.slice("persona:".length));
      return p?.display_name ?? speakerId.slice("persona:".length);
    }
    if (speakerId.startsWith("ext:")) return "外部の声";
    return "人間"; // Discord ユーザ (= 人間参加者)
  }

  function isHumanSpeaker(speakerId: string | null): boolean {
    return !!speakerId && !speakerId.startsWith("persona:") && !speakerId.startsWith("ext:");
  }

  function gapTopic(gapId: string): string {
    const g = raw
      .prepare("SELECT title, description FROM design_gaps WHERE id = ?")
      .get(gapId) as { title: string; description: string | null } | undefined;
    const base = g ? `${g.title}\n${g.description ?? ""}`.trim() : "(不明な議題)";
    // 参加者からの進行調整指示があれば topic 末尾に注入し、 expand/converge/止揚判定の
    // 全 prompt (gapTopic を読む) に効かせる。
    const directives = listFacilitatorDirectives(raw, deps.workspaceId, gapId);
    const block = formatDirectivesBlock(directives);
    return block ? `${base}\n\n${block}` : base;
  }

  function existingPersonaNames(sessionId: string): string[] {
    const rows = raw
      .prepare(
        "SELECT DISTINCT speaker_id FROM utterances WHERE session_id = ? AND speaker_id LIKE 'persona:%'"
      )
      .all(sessionId) as Array<{ speaker_id: string }>;
    return rows.map((r) => speakerLabel(r.speaker_id));
  }

  function listAufhebung(gapId: string): string[] {
    const rows = raw
      .prepare("SELECT summary FROM aufhebung_stock WHERE gap_id = ? ORDER BY created_at ASC")
      .all(gapId) as Array<{ summary: string }>;
    return rows.map((r) => r.summary);
  }

  function stockAufhebung(gapId: string, summary: string): void {
    raw
      .prepare("INSERT INTO aufhebung_stock (id, gap_id, summary, created_at) VALUES (?, ?, ?, ?)")
      .run(randomUUID(), gapId, summary, Date.now());
  }

  // 人間の意見はリアクションが無くても収束まとめで優先的に拾うためのスコア下駄。
  // 「人間がいる場合は他の AI の意見より優先度を高く」 を反映する。
  const HUMAN_OPINION_BASE = 5;

  /** session 内発話を スコア降順で上位 (まとめがスコアに引っ張られる)。 人間発言は下駄あり。 */
  function topOpinions(sessionId: string, limit: number): string[] {
    const rows = raw
      .prepare(
        "SELECT id, speaker_id, raw_content FROM utterances WHERE session_id = ? AND speaker_id IS NOT NULL AND id NOT IN (SELECT utterance_id FROM utterance_exclusions)"
      )
      .all(sessionId) as Array<{ id: string; speaker_id: string; raw_content: string }>;
    return rows
      .filter((u) => u.speaker_id.startsWith("persona:") || isHumanSpeaker(u.speaker_id))
      .map((u) => {
        const human = isHumanSpeaker(u.speaker_id);
        return { ...u, human, score: getOpinionScore(raw, u.id) + (human ? HUMAN_OPINION_BASE : 0) };
      })
      .filter((u) => u.score > 0)
      .sort((a, b) => b.score - a.score)
      .slice(0, limit)
      .map(
        (u) =>
          `${speakerLabel(u.speaker_id)}「${u.raw_content}」(+${u.score}${u.human ? " / 人間" : ""})`
      );
  }

  /** 止揚判定 → 新規なら stock。 stock 後の総数を返す。 */
  async function judgeAndStockAufhebung(d: Discussion): Promise<number> {
    const stocked = listAufhebung(d.gapId);
    const { system, user } = buildAufhebungJudgePrompt({
      topic: gapTopic(d.gapId),
      recent: recentUtterances(d.sessionId, 16),
      stockedSummaries: stocked,
    });
    const model =
      resolveTierModel({ kind: "aufhebung" }, deps.options?.tierModels) ?? deps.options?.model;
    const res = await deps.llm.invoke({ prompt: user, system, model });
    if (!res.ok) return stocked.length;
    try {
      const obj = extractJsonObject(res.text) as { found?: unknown; summary?: unknown };
      const summary = typeof obj.summary === "string" ? obj.summary.trim() : "";
      if (obj.found === true && summary && !stocked.some((s) => s === summary)) {
        stockAufhebung(d.gapId, summary);
        deps.logger.info({ gap_id: d.gapId, summary }, "facilitator stocked aufhebung");
        return stocked.length + 1;
      }
    } catch {
      /* 判定失敗は無視 */
    }
    return stocked.length;
  }

  async function expand(d: Discussion): Promise<void> {
    const { system, user } = buildExpandPrompt({
      topic: gapTopic(d.gapId),
      existingPersonaNames: existingPersonaNames(d.sessionId),
      recent: recentUtterances(d.sessionId, 12),
    });
    const model =
      resolveTierModel({ kind: "facilitate" }, deps.options?.tierModels) ?? deps.options?.model;
    const res = await deps.llm.invoke({ prompt: user, system, model });
    if (!res.ok) {
      deps.logger.warn({ gap_id: d.gapId, err: res.error }, "facilitator expand llm failed");
      return;
    }
    let gen: GeneratedPersona;
    try {
      gen = extractJsonObject(res.text) as GeneratedPersona;
    } catch (err) {
      deps.logger.warn({ gap_id: d.gapId, err: (err as Error).message }, "facilitator expand parse failed");
      return;
    }
    if (!gen.opening || typeof gen.opening !== "string") return;
    const personaId = `dyn-${randomUUID().slice(0, 8)}`;
    deps.personas.insertOrIgnore({
      id: personaId,
      name: String(gen.name ?? "新視点"),
      display_name: String(gen.display_name ?? "名無し"),
      description: String(gen.description ?? ""),
      traits: Array.isArray(gen.traits) ? gen.traits.map(String) : [],
      speech_style: String(gen.speech_style ?? ""),
    });
    deps.contextProvider.postUtterance({
      workspaceId: deps.workspaceId,
      sessionId: d.sessionId,
      text: gen.opening,
      byPersonaId: personaId,
    });
    deps.logger.info({ gap_id: d.gapId, persona_id: personaId, name: gen.name }, "facilitator expanded discussion");
  }

  async function converge(d: Discussion): Promise<void> {
    const { system, user } = buildConvergePrompt({
      topic: gapTopic(d.gapId),
      recent: recentUtterances(d.sessionId, 40),
      aufhebungen: listAufhebung(d.gapId),
      topOpinions: topOpinions(d.sessionId, 5),
    });
    const model =
      resolveTierModel({ kind: "converge" }, deps.options?.tierModels) ?? deps.options?.model;
    const res = await deps.llm.invoke({ prompt: user, system, model });
    if (!res.ok) {
      deps.logger.warn({ gap_id: d.gapId, err: res.error }, "facilitator converge llm failed");
      return;
    }
    let summary: string;
    try {
      const obj = extractJsonObject(res.text) as { summary?: unknown };
      summary = typeof obj.summary === "string" ? obj.summary : res.text;
    } catch {
      summary = res.text;
    }
    deps.contextProvider.postUtterance({
      workspaceId: deps.workspaceId,
      sessionId: d.sessionId,
      text: `【収束】\n${summary}`,
      byPersonaId: FACILITATOR_PERSONA_ID,
    });
    closeGap(d.gapId);
    // gap closed と session ライフサイクルを揃える: 収束した議論の session を ended_at で
    // 閉じる (admin の手動 close と同じ scene 単位)。これをしないと自走収束した議論が
    // sessions.ended_at=NULL のまま残り、 ダッシュボードの「進行中の議論 session」に
    // 出続ける (gap は closed なのに session は開きっぱなし、という乖離)。
    const scene = endConvergedSessions(d.sessionId);
    states.delete(d.sessionId); // 収束したら管理終了 (= ファシリテーター消滅)
    deps.logger.info({ gap_id: d.gapId }, "facilitator converged discussion (gap closed)");

    // 収束フック (フォーラム集約: スレッドを締めてまとめを転記)。失敗は議論を止めない。
    if (deps.onConverged) {
      try {
        deps.onConverged({
          gapId: d.gapId,
          sessionId: d.sessionId,
          scene,
          title: gapTitle(d.gapId),
          summary,
        });
      } catch (err) {
        deps.logger.warn({ gap_id: d.gapId, err: (err as Error).message }, "facilitator onConverged hook failed");
      }
    }
  }

  function gapTitle(gapId: string): string {
    const g = raw.prepare("SELECT title FROM design_gaps WHERE id = ?").get(gapId) as
      | { title: string }
      | undefined;
    return g?.title ?? "(無題の議論)";
  }

  /**
   * 収束した議論の session を ended_at で閉じる (admin の close と同じ scene 単位)。
   * intake session と discussion-of-gap session は同一 scene を共有するため両方終了する。
   * @returns 当該 session の scene (onConverged フックへ引き渡す。 scene 無しは null)
   */
  function endConvergedSessions(sessionId: string): string | null {
    const scene =
      (
        raw.prepare("SELECT scene FROM sessions WHERE id = ?").get(sessionId) as
          | { scene: string | null }
          | undefined
      )?.scene ?? null;
    const now = Date.now();
    if (scene) {
      raw
        .prepare(
          "UPDATE sessions SET ended_at = ?, updated_at = ? WHERE workspace_id = ? AND scene = ? AND ended_at IS NULL"
        )
        .run(now, now, deps.workspaceId, scene);
    } else {
      raw
        .prepare("UPDATE sessions SET ended_at = ?, updated_at = ? WHERE id = ? AND ended_at IS NULL")
        .run(now, now, sessionId);
    }
    return scene;
  }

  function closeGap(gapId: string): void {
    const g = raw
      .prepare("SELECT title, description FROM design_gaps WHERE id = ?")
      .get(gapId) as { title: string; description: string | null } | undefined;
    if (!g) return;
    deps.core.repos.designGap.update(gapId, {
      workspaceId: deps.workspaceId,
      title: g.title,
      description: g.description ?? undefined,
      status: "closed",
    });
  }

  /** idle 時の介入: 止揚判定 → たまっていれば収束、 まだなら拡張。 */
  async function intervene(d: Discussion, metrics: SessionMetrics): Promise<void> {
    const t = getTuning();
    const aufCount = await judgeAndStockAufhebung(d);
    // aufhebung-only ポリシーでは persona 数では収束させず、 止揚が出るまで続ける。
    const personaOver =
      t.convergePolicy === "aufhebung-only" ? false : metrics.distinctPersonas > t.maxPersonas;
    if (aufCount >= t.aufhebungTarget || personaOver) {
      await converge(d);
    } else {
      await expand(d);
    }
  }

  async function tickOnce(): Promise<void> {
    const now = Date.now();
    for (const d of listActiveDiscussions()) {
      const metrics = metricsFor(d.sessionId, now);
      const prev = states.get(d.sessionId) ?? { lastUtteranceCount: metrics.utteranceCount, armed: true };
      const { mode, state } = evaluate(metrics, prev, { idleGapMs: getTuning().idleGapMs });
      states.set(d.sessionId, state);
      try {
        if (mode === "intervene") await intervene(d, metrics);
      } catch (err) {
        deps.logger.warn({ gap_id: d.gapId, err: (err as Error).message }, "facilitator action failed");
      }
    }
  }

  return {
    start(): void {
      if (timer) return;
      timer = setInterval(() => {
        if (running) return;
        running = true;
        tickOnce()
          .catch((err) => deps.logger.warn({ err: (err as Error).message }, "facilitator tick failed"))
          .finally(() => {
            running = false;
          });
      }, tickMs);
    },
    stop(): void {
      if (timer) clearInterval(timer);
      timer = null;
    },
    tickOnce,
    async convergeNow(input: { sessionId: string; gapId: string }): Promise<{ ok: boolean; error?: string }> {
      const gap = raw
        .prepare("SELECT status FROM design_gaps WHERE id = ?")
        .get(input.gapId) as { status: string | null } | undefined;
      if (!gap) return { ok: false, error: "gap not found" };
      if (gap.status && ["closed", "converged", "dismissed"].includes(gap.status)) {
        return { ok: false, error: `already ${gap.status}` };
      }
      try {
        await converge({ sessionId: input.sessionId, gapId: input.gapId });
      } catch (e) {
        return { ok: false, error: (e as Error).message };
      }
      // converge は LLM 失敗時に早期 return する (gap を閉じない) ので、 実際に
      // closed になったかで成否を判定する (偽 ok を返さない)。
      const after = raw
        .prepare("SELECT status FROM design_gaps WHERE id = ?")
        .get(input.gapId) as { status: string | null } | undefined;
      if (after?.status === "closed" || after?.status === "converged") return { ok: true };
      return { ok: false, error: "まとめ生成に失敗しました (LLM 応答なし)。再試行してください" };
    },
  };
}
