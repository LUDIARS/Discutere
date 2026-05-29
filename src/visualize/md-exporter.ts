/**
 * Discatier Core ノード → markdown レンダ (Phase 0)
 *
 * 各 exportXxx(core, ws, id) は frontmatter (YAML) + 本文 markdown を組み立てる。
 * 参照は wikilink (`[[type:id]]`) で埋め込む。 隣接ノードの md は生成しない
 * (= depth=0)。 traversal は dump.ts。
 *
 * 失敗時は ExporterError を throw (CLI 側で catch + exit code 1)。
 */

import matter from "gray-matter";

import type { createCore } from "../core/index.js";

import { formatWikilink, type NodeType, type Wikilink } from "./wikilink.js";

type Core = ReturnType<typeof createCore>;

export class ExporterError extends Error {
  constructor(message: string, public readonly type?: NodeType, public readonly id?: string) {
    super(message);
    this.name = "ExporterError";
  }
}

export interface ExportResult {
  type: NodeType;
  id: string;
  markdown: string;
  /** 本ノードから出る wikilink (重複あり) — dump.ts の traversal で使う */
  outgoing: Wikilink[];
}

// ─────────────────────────── Hypothesis ───────────────────────────

interface HypothesisRow {
  id: string;
  workspace_id: string;
  design_gap_id: string | null;
  statement: string;
  status: string | null;
  integrated: number;
  validated_by_emotion: number;
  refs_json: string | null;
  stale_flagged: number;
  created_at: number;
  updated_at: number;
}

export function exportHypothesis(core: Core, ws: string, id: string): ExportResult {
  const row = core.client.raw
    .prepare("SELECT * FROM hypotheses WHERE workspace_id = ? AND id = ?")
    .get(ws, id) as HypothesisRow | undefined;
  if (!row) throw new ExporterError(`hypothesis not found: ${id}`, "hyp", id);

  const refs = parseRefsJson(row.refs_json).map((uttId) => ({ type: "utt" as const, id: uttId }));
  const addresses: Wikilink | undefined = row.design_gap_id
    ? { type: "gap", id: row.design_gap_id }
    : undefined;

  // 派生 mechanic: mechanics.intends に hypothesis.statement が含まれるかで素朴に逆引き
  // (Phase 0 の暫定。 Phase 1 で events log から正確に追跡する)
  const proposedMechanics = core.client.raw
    .prepare(
      "SELECT id, name FROM mechanics WHERE workspace_id = ? AND intends LIKE ?"
    )
    .all(ws, `%${row.statement.slice(0, 32)}%`) as Array<{ id: string; name: string }>;
  const proposes = proposedMechanics.map((m) => ({ type: "mch" as const, id: m.id }));

  const validatedBy: string[] = [];
  if (row.validated_by_emotion === 1) validatedBy.push("emotion");

  const fm: Record<string, unknown> = {
    id: row.id,
    type: "hypothesis",
    workspace_id: row.workspace_id,
    status: row.status ?? "proposed",
    statement: row.statement,
    integrated: row.integrated === 1,
    stale_flagged: row.stale_flagged === 1,
    created_at: tsToIso(row.created_at),
    updated_at: tsToIso(row.updated_at),
  };
  if (addresses) fm.addresses = formatWikilink(addresses);
  if (proposes.length > 0) fm.proposes = proposes.map(formatWikilink);
  if (refs.length > 0) fm.refs = refs.map(formatWikilink);
  if (validatedBy.length > 0) fm.validated_by = validatedBy;

  const lines: string[] = [];
  lines.push(`# 仮説: ${row.statement}`);
  lines.push("");
  lines.push(`## ステータス`);
  lines.push(
    `**${row.status ?? "proposed"}**${
      validatedBy.length > 0 ? ` (validated_by: ${validatedBy.join(", ")})` : ""
    }${row.integrated === 1 ? " / integrated" : ""}${
      row.stale_flagged === 1 ? " / stale" : ""
    }`
  );

  if (refs.length > 0) {
    lines.push("");
    lines.push(`## 根拠 (utterances)`);
    for (const ref of refs) {
      lines.push(`- ${formatWikilink(ref)}`);
    }
  }

  if (addresses) {
    lines.push("");
    lines.push(`## 取り組む gap`);
    lines.push(formatWikilink(addresses));
  }

  if (proposes.length > 0) {
    lines.push("");
    lines.push(`## 派生 mechanic`);
    for (const m of proposes) {
      lines.push(`- ${formatWikilink(m)}`);
    }
  }

  const md = matter.stringify(`\n${lines.join("\n")}\n`, fm);
  const outgoing: Wikilink[] = [...refs, ...proposes, ...(addresses ? [addresses] : [])];
  return { type: "hyp", id, markdown: md, outgoing };
}

// ─────────────────────────── Design Gap ───────────────────────────

interface GapRow {
  id: string;
  workspace_id: string;
  game_id: string | null;
  title: string;
  description: string | null;
  status: string | null;
  gap_in: string | null;
  expected_affect: string | null;
  observed_affect: string | null;
  evidence_json: string | null;
  created_at: number;
  updated_at: number;
}

export function exportGap(core: Core, ws: string, id: string): ExportResult {
  const row = core.client.raw
    .prepare("SELECT * FROM design_gaps WHERE workspace_id = ? AND id = ?")
    .get(ws, id) as GapRow | undefined;
  if (!row) throw new ExporterError(`design gap not found: ${id}`, "gap", id);

  // この gap を addresses している hypothesis を逆引き
  const hyps = core.client.raw
    .prepare(
      "SELECT id FROM hypotheses WHERE workspace_id = ? AND design_gap_id = ? ORDER BY updated_at DESC"
    )
    .all(ws, id) as Array<{ id: string }>;
  const hypotheses: Wikilink[] = hyps.map((h) => ({ type: "hyp", id: h.id }));

  const fm: Record<string, unknown> = {
    id: row.id,
    type: "gap",
    workspace_id: row.workspace_id,
    title: row.title,
    status: row.status ?? "open",
    created_at: tsToIso(row.created_at),
    updated_at: tsToIso(row.updated_at),
  };
  if (row.gap_in) fm.gap_in = row.gap_in;
  if (row.expected_affect) fm.expected_affect = row.expected_affect;
  if (row.observed_affect) fm.observed_affect = row.observed_affect;
  if (row.game_id) fm.game = formatWikilink({ type: "game", id: row.game_id });
  if (hypotheses.length > 0) fm.hypotheses = hypotheses.map(formatWikilink);

  const lines: string[] = [];
  lines.push(`# Design Gap: ${row.title}`);
  if (row.description) {
    lines.push("");
    lines.push(row.description);
  }
  if (row.expected_affect || row.observed_affect) {
    lines.push("");
    lines.push(`## 期待 vs 観測`);
    lines.push(
      `- 期待: **${row.expected_affect ?? "?"}**${row.gap_in ? ` (in ${row.gap_in})` : ""}`
    );
    lines.push(`- 観測: **${row.observed_affect ?? "?"}**`);
  }
  if (hypotheses.length > 0) {
    lines.push("");
    lines.push(`## 提案された hypotheses`);
    for (const h of hypotheses) {
      lines.push(`- ${formatWikilink(h)}`);
    }
  }

  const md = matter.stringify(`\n${lines.join("\n")}\n`, fm);
  const outgoing: Wikilink[] = [...hypotheses];
  if (row.game_id) outgoing.push({ type: "game", id: row.game_id });
  return { type: "gap", id, markdown: md, outgoing };
}

// ─────────────────────────── Mechanic ───────────────────────────

interface MechanicRow {
  id: string;
  workspace_id: string;
  game_id: string | null;
  name: string;
  description: string | null;
  intends: string | null;
  intended_affect: string | null;
  created_at: number;
  updated_at: number;
}

export function exportMechanic(core: Core, ws: string, id: string): ExportResult {
  const row = core.client.raw
    .prepare("SELECT * FROM mechanics WHERE workspace_id = ? AND id = ?")
    .get(ws, id) as MechanicRow | undefined;
  if (!row) throw new ExporterError(`mechanic not found: ${id}`, "mch", id);

  const fm: Record<string, unknown> = {
    id: row.id,
    type: "mechanic",
    workspace_id: row.workspace_id,
    name: row.name,
    created_at: tsToIso(row.created_at),
    updated_at: tsToIso(row.updated_at),
  };
  if (row.description) fm.description = row.description;
  if (row.intends) fm.intends = row.intends;
  if (row.intended_affect) fm.intended_affect = row.intended_affect;
  const gameLink: Wikilink | undefined = row.game_id ? { type: "game", id: row.game_id } : undefined;
  if (gameLink) fm.game = formatWikilink(gameLink);

  const lines: string[] = [];
  lines.push(`# Mechanic: ${row.name}`);
  if (row.description) {
    lines.push("");
    lines.push(row.description);
  }
  if (row.intends) {
    lines.push("");
    lines.push(`## 設計意図`);
    lines.push(row.intends);
  }
  if (row.intended_affect) {
    lines.push("");
    lines.push(`## 狙う affect`);
    lines.push(`**${row.intended_affect}**`);
  }
  if (gameLink) {
    lines.push("");
    lines.push(`## ゲーム`);
    lines.push(formatWikilink(gameLink));
  }

  const md = matter.stringify(`\n${lines.join("\n")}\n`, fm);
  const outgoing: Wikilink[] = gameLink ? [gameLink] : [];
  return { type: "mch", id, markdown: md, outgoing };
}

// ─────────────────────────── Aesthetic ───────────────────────────

interface AestheticRow {
  id: string;
  workspace_id: string;
  game_id: string | null;
  name: string;
  description: string | null;
  created_at: number;
  updated_at: number;
}

export function exportAesthetic(core: Core, ws: string, id: string): ExportResult {
  const row = core.client.raw
    .prepare("SELECT * FROM aesthetics WHERE workspace_id = ? AND id = ?")
    .get(ws, id) as AestheticRow | undefined;
  if (!row) throw new ExporterError(`aesthetic not found: ${id}`, "aes", id);

  const fm: Record<string, unknown> = {
    id: row.id,
    type: "aesthetic",
    workspace_id: row.workspace_id,
    name: row.name,
    created_at: tsToIso(row.created_at),
    updated_at: tsToIso(row.updated_at),
  };
  if (row.description) fm.description = row.description;
  const gameLink: Wikilink | undefined = row.game_id ? { type: "game", id: row.game_id } : undefined;
  if (gameLink) fm.game = formatWikilink(gameLink);

  const lines: string[] = [];
  lines.push(`# Aesthetic: ${row.name}`);
  if (row.description) {
    lines.push("");
    lines.push(row.description);
  }
  if (gameLink) {
    lines.push("");
    lines.push(`## ゲーム`);
    lines.push(formatWikilink(gameLink));
  }
  const md = matter.stringify(`\n${lines.join("\n")}\n`, fm);
  return { type: "aes", id, markdown: md, outgoing: gameLink ? [gameLink] : [] };
}

// ─────────────────────────── Utterance ───────────────────────────

interface UtteranceRow {
  id: string;
  workspace_id: string;
  session_id: string;
  speaker_id: string | null;
  raw_content: string;
  normalized_content: string | null;
  responds_to: string | null;
  posted_at: number;
  created_at: number;
  updated_at: number;
}

export function exportUtterance(core: Core, ws: string, id: string): ExportResult {
  const row = core.client.raw
    .prepare("SELECT * FROM utterances WHERE workspace_id = ? AND id = ?")
    .get(ws, id) as UtteranceRow | undefined;
  if (!row) throw new ExporterError(`utterance not found: ${id}`, "utt", id);

  const sessionLink: Wikilink = { type: "ses", id: row.session_id };
  const respondsToLink: Wikilink | undefined = row.responds_to
    ? { type: "utt", id: row.responds_to }
    : undefined;
  // この utterance を refs に持つ hypothesis を逆引き (refs_json に id 含む)
  const referencedBy = core.client.raw
    .prepare(
      "SELECT id FROM hypotheses WHERE workspace_id = ? AND refs_json LIKE ? ORDER BY updated_at DESC"
    )
    .all(ws, `%${id}%`) as Array<{ id: string }>;
  const refLinks: Wikilink[] = referencedBy.map((h) => ({ type: "hyp", id: h.id }));

  const fm: Record<string, unknown> = {
    id: row.id,
    type: "utterance",
    workspace_id: row.workspace_id,
    session: formatWikilink(sessionLink),
    speaker: row.speaker_id ?? null,
    posted_at: tsToIso(row.posted_at),
    created_at: tsToIso(row.created_at),
  };
  if (respondsToLink) fm.responds_to = formatWikilink(respondsToLink);
  if (refLinks.length > 0) fm.referenced_by = refLinks.map(formatWikilink);

  const lines: string[] = [];
  lines.push(`# Utterance`);
  lines.push("");
  for (const line of row.raw_content.split("\n")) {
    lines.push(`> ${line}`);
  }
  if (row.normalized_content && row.normalized_content !== row.raw_content) {
    lines.push("");
    lines.push(`## 正規化`);
    lines.push(row.normalized_content);
  }
  if (refLinks.length > 0) {
    lines.push("");
    lines.push(`## 参照している hypothesis`);
    for (const h of refLinks) {
      lines.push(`- ${formatWikilink(h)}`);
    }
  }

  const md = matter.stringify(`\n${lines.join("\n")}\n`, fm);
  const outgoing: Wikilink[] = [sessionLink, ...refLinks];
  if (respondsToLink) outgoing.push(respondsToLink);
  return { type: "utt", id, markdown: md, outgoing };
}

// ─────────────────────────── Session ───────────────────────────

interface SessionRow {
  id: string;
  workspace_id: string;
  title: string;
  started_at: number;
  ended_at: number | null;
  mode: string | null;
  scene: string | null;
  created_at: number;
  updated_at: number;
}

export function exportSession(core: Core, ws: string, id: string): ExportResult {
  const row = core.client.raw
    .prepare("SELECT * FROM sessions WHERE workspace_id = ? AND id = ?")
    .get(ws, id) as SessionRow | undefined;
  if (!row) throw new ExporterError(`session not found: ${id}`, "ses", id);

  const utts = core.client.raw
    .prepare(
      "SELECT id FROM utterances WHERE workspace_id = ? AND session_id = ? ORDER BY posted_at"
    )
    .all(ws, id) as Array<{ id: string }>;
  const uttLinks: Wikilink[] = utts.map((u) => ({ type: "utt", id: u.id }));

  const fm: Record<string, unknown> = {
    id: row.id,
    type: "session",
    workspace_id: row.workspace_id,
    title: row.title,
    started_at: tsToIso(row.started_at),
    created_at: tsToIso(row.created_at),
    updated_at: tsToIso(row.updated_at),
  };
  if (row.ended_at) fm.ended_at = tsToIso(row.ended_at);
  if (row.mode) fm.mode = row.mode;
  if (row.scene) fm.scene = row.scene;
  if (uttLinks.length > 0) fm.utterances = uttLinks.map(formatWikilink);

  const lines: string[] = [];
  lines.push(`# Session: ${row.title}`);
  if (row.mode || row.scene) {
    lines.push("");
    lines.push(`- mode: ${row.mode ?? "?"}`);
    if (row.scene) lines.push(`- scene: ${row.scene}`);
  }
  if (uttLinks.length > 0) {
    lines.push("");
    lines.push(`## Utterances (${uttLinks.length} 件)`);
    for (const u of uttLinks) {
      lines.push(`- ${formatWikilink(u)}`);
    }
  }

  const md = matter.stringify(`\n${lines.join("\n")}\n`, fm);
  return { type: "ses", id, markdown: md, outgoing: uttLinks };
}

// ─────────────────────────── Dispatcher ───────────────────────────

export type Exporter = (core: Core, ws: string, id: string) => ExportResult;

export const EXPORTERS: Record<NodeType, Exporter | null> = {
  hyp: exportHypothesis,
  gap: exportGap,
  mch: exportMechanic,
  aes: exportAesthetic,
  utt: exportUtterance,
  ses: exportSession,
  // affect / game は Phase 0 では未対応 (game は crawler 側で md 既に生成、 affect は使用頻度低)
  aff: null,
  game: null,
};

export function exportNode(core: Core, type: NodeType, ws: string, id: string): ExportResult {
  const fn = EXPORTERS[type];
  if (!fn) {
    throw new ExporterError(`exporter not implemented for type: ${type}`, type, id);
  }
  return fn(core, ws, id);
}

// ─────────────────────────── helpers ───────────────────────────

function parseRefsJson(raw: string | null): string[] {
  if (!raw) return [];
  try {
    const parsed = JSON.parse(raw);
    if (Array.isArray(parsed)) return parsed.filter((x) => typeof x === "string");
  } catch {
    /* ignore malformed */
  }
  return [];
}

function tsToIso(ts: number): string {
  return new Date(ts).toISOString();
}
