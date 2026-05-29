/**
 * Persona md exporter (PR-E / MISSING #5-1).
 *
 * persona は persona-engine の独立 DB (PersonasRepo) に保存されているので、
 * Discatier core 用の md-exporter とは分離して inject パターンで実装。
 *
 * 出力先: data/discussions/<workspace>/persona/<id>.md
 * (= visualize の TYPE_TO_DIR.persona 規約と一致)
 */

import fs from "node:fs";
import path from "node:path";
import matter from "gray-matter";

import type { PersonaRow, PersonasRepo } from "../persona-engine/index.js";
import { TYPE_TO_DIR } from "./wikilink.js";

export interface PersonaExportResult {
  id: string;
  markdown: string;
  filePath?: string;
  written?: "created" | "updated" | "unchanged";
}

export function exportPersona(
  personas: PersonasRepo,
  id: string,
  workspaceId: string
): PersonaExportResult {
  const p = personas.get(id);
  if (!p) throw new Error(`persona not found: ${id}`);

  const md = renderPersonaMd(p, workspaceId);
  return { id: p.id, markdown: md };
}

export function dumpPersona(
  personas: PersonasRepo,
  id: string,
  options: { rootDir?: string; workspaceId: string }
): PersonaExportResult {
  const result = exportPersona(personas, id, options.workspaceId);
  const root = options.rootDir ?? path.resolve("data/discussions");
  const filePath = path.join(root, options.workspaceId, TYPE_TO_DIR.persona, `${id}.md`);
  fs.mkdirSync(path.dirname(filePath), { recursive: true });

  let written: PersonaExportResult["written"];
  if (fs.existsSync(filePath)) {
    const current = fs.readFileSync(filePath, "utf8");
    if (current === result.markdown) {
      written = "unchanged";
    } else {
      fs.writeFileSync(filePath, result.markdown);
      written = "updated";
    }
  } else {
    fs.writeFileSync(filePath, result.markdown);
    written = "created";
  }
  return { ...result, filePath, written };
}

function renderPersonaMd(p: PersonaRow, workspaceId: string): string {
  const traits = safeJsonArrayString(p.traits);
  const notes = safeJsonNotes(p.learned_notes);
  const fm: Record<string, unknown> = {
    id: p.id,
    type: "persona",
    workspace_id: workspaceId,
    name: p.name,
    display_name: p.display_name,
    description: p.description,
    traits,
    speech_style: p.speech_style,
    created_at: tsSecToIso(p.created_at),
    updated_at: tsSecToIso(p.updated_at),
  };
  if (notes.length > 0) {
    fm.learned_notes_count = notes.length;
  }

  const lines: string[] = [];
  lines.push(`# ${p.name} (${p.display_name})`);
  lines.push("");
  lines.push(p.description);
  if (traits.length > 0) {
    lines.push("");
    lines.push(`## 特徴`);
    for (const t of traits) lines.push(`- ${t}`);
  }
  if (p.speech_style) {
    lines.push("");
    lines.push(`## 話し方`);
    lines.push(p.speech_style);
  }
  if (notes.length > 0) {
    lines.push("");
    lines.push(`## 学び (${notes.length} 件、 最新 5 件)`);
    for (const n of notes.slice(-5).reverse()) {
      const ts = tsSecToIso(n.ts);
      lines.push(`- \`${ts}\` — ${n.note}`);
    }
  }
  return matter.stringify(`\n${lines.join("\n")}\n`, fm);
}

function safeJsonArrayString(raw: string): string[] {
  try {
    const v = JSON.parse(raw);
    if (Array.isArray(v)) return v.filter((x) => typeof x === "string");
  } catch {
    /* ignore */
  }
  return [];
}

interface LearnedNote {
  note: string;
  ts: number;
}

function safeJsonNotes(raw: string): LearnedNote[] {
  try {
    const v = JSON.parse(raw);
    if (Array.isArray(v)) {
      return v.filter(
        (x): x is LearnedNote =>
          x && typeof x.note === "string" && typeof x.ts === "number"
      );
    }
  } catch {
    /* ignore */
  }
  return [];
}

function tsSecToIso(tsSec: number): string {
  return new Date(tsSec * 1000).toISOString();
}
