import { randomUUID } from "node:crypto";
import type { ReturnTypeCreateCore } from "../../projection/types.js";
import { shouldDetectGap } from "./matcher.js";
import { existsGap } from "./dedup.js";

export function detectGaps(core: ReturnTypeCreateCore, workspaceId: string): string[] {
  const created: string[] = [];
  const mechanics = core.client.raw.prepare("SELECT id, name, game_id, intended_affect FROM mechanics WHERE workspace_id = ? AND intended_affect IS NOT NULL").all(workspaceId) as any[];

  const slugify = (s: string) => (s || "").toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-|-$/g, "");
  // workspace 全体の観測 affect (payload_json.source = ゲーム slug)。
  const wsRows = core.client.raw.prepare(
    `SELECT tp.target_name AS observed, u.id AS utterance_id, tp.payload_json AS payload
     FROM utterances u
     JOIN translation_proposals tp ON tp.utterance_id = u.id AND tp.proposal_type = 'affect' AND tp.status IN ('approved','revised')
     WHERE u.workspace_id = ?`
  ).all(workspaceId) as Array<{ observed: string; utterance_id: string; payload: string | null }>;

  for (const m of mechanics) {
    // game-scoped: mechanic の game (title→slug) に紐づく観測のみで判定。
    // game / source が取れない場合は workspace 全体にフォールバック (既存挙動・テスト互換)。
    let slug: string | null = null;
    if (m.game_id) {
      const g = core.client.raw.prepare("SELECT title FROM games WHERE id = ?").get(m.game_id) as { title?: string } | undefined;
      if (g?.title) slug = slugify(g.title);
    }
    // game が分かるなら厳密に game-scoped (フォールバックしない → 観測ゼロのゲームは Gap 化しない)。
    // game 不明 (game_id なし) のときのみ workspace 全体 (テスト互換)。
    const observedRows = slug
      ? wsRows.filter((r) => { try { return !!r.payload && JSON.parse(r.payload).source === slug; } catch { return false; } })
      : wsRows;
    const observed = observedRows.map((r) => r.observed).filter(Boolean) as string[];
    if (observed.length < 3) continue;

    const decision = shouldDetectGap({ intended: m.intended_affect, observed });
    if (!decision.detect) continue;

    const observedAffect = decision.type === "missing" ? "(missing)" : observed[0];
    if (existsGap(core, { workspaceId, mechanicId: m.id, expectedAffect: m.intended_affect, observedAffect })) continue;

    const evidence = observedRows.slice(0, 3).map((r) => r.utterance_id);
    const id = randomUUID();
    core.client.raw.prepare(
      `INSERT INTO design_gaps (id, workspace_id, game_id, title, description, status, gap_in, expected_affect, observed_affect, evidence_json, created_at, updated_at)
       VALUES (?, ?, ?, ?, ?, 'open', ?, ?, ?, ?, ?, ?)`
    ).run(
      id,
      workspaceId,
      m.game_id ?? null,
      `Gap: ${m.name}`,
      `detected by matcher (${decision.type})`,
      m.id,
      m.intended_affect,
      observedAffect,
      JSON.stringify(evidence),
      Date.now(),
      Date.now()
    );
    created.push(id);
  }
  return created;
}
