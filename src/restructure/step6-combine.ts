/**
 * ⑥ 他ゲームの高評価メカニクスを結合。
 *
 * KG 横断で高評価な mechanic を候補として抽出し、
 * (1) playContext 両立 (2) 嗜好適合 (3) 機能的補完 の AND 条件でフィルタする。
 *
 * spec/RESTRUCTURE.md §2-⑥ / §4-D。
 */

import { readdir, readFile } from "node:fs/promises";
import { join } from "node:path";
import { randomUUID } from "node:crypto";
import type {
  GameIntake,
  Step4Output,
  Step5Output,
  PreferenceProfile,
  CombinationCandidate,
  MechanicMid,
  Step6Output,
} from "./types.js";
import { findProfile, cosine } from "./step1-preferences.js";

const CANDIDATE_SCORE_THRESHOLD = 0.6;
const PREFERENCE_COMPAT_THRESHOLD = 0.4;
const MAX_CANDIDATES_PER_GAME = 3;

interface OtherGameMechanic {
  slug: string;
  title: string;
  mechanic: MechanicMid;
  /** step4 の micro 評価スコアがあれば使う。なければ 0.5 */
  score: number;
}

// ── 他ゲームのメカニクスを列挙 ───────────────────────────────────────────────

async function loadOtherGameMechanics(
  gamesDir: string,
  restructureDir: string,
  excludeSlug: string,
): Promise<OtherGameMechanic[]> {
  const results: OtherGameMechanic[] = [];
  const mdFiles = (await readdir(gamesDir).catch(() => [])).filter((f) => f.endsWith(".md") && !f.includes("sample"));

  for (const mdFile of mdFiles) {
    const slug = mdFile.replace(/\.md$/, "");
    if (slug === excludeSlug) continue;

    const mdRaw = await readFile(join(gamesDir, mdFile), "utf-8").catch(() => "");
    const title = /^title:\s*["']?(.+?)["']?\s*$/m.exec(mdRaw)?.[1]?.trim() ?? slug;

    // mechanics を frontmatter から抽出
    const mechanics = parseMechanics(mdRaw);

    // step4 の評価があれば読む
    let step4: Step4Output | null = null;
    try {
      const step4Raw = await readFile(join(restructureDir, slug, "step4.json"), "utf-8");
      step4 = JSON.parse(step4Raw) as Step4Output;
    } catch {
      // 評価がなければ score=0.5 で扱う
    }

    for (const mechanic of mechanics) {
      const score = step4?.micro.find((ev) => ev.mechanicName === mechanic.name)?.score ?? 0.5;
      results.push({ slug, title, mechanic, score });
    }
  }

  return results;
}

function parseMechanics(mdContent: string): MechanicMid[] {
  const content = mdContent.replace(/\r\n/g, "\n");
  const match = /^---\n([\s\S]*?)\n---/.exec(content);
  if (!match) return [];

  const lines = match[1].split("\n");
  const mechanics: MechanicMid[] = [];
  let inMechanics = false;
  let current: Partial<MechanicMid> | null = null;

  const flush = () => {
    if (current?.name) mechanics.push({ id: randomUUID(), ...current } as MechanicMid);
    current = null;
  };

  for (const line of lines) {
    const scalar = (v: string) => v.replace(/^["']|["']$/g, "").trim();
    const topKey = /^(\w[\w_]*):\s*(.*)/.exec(line);
    if (topKey && !line.startsWith(" ") && !line.startsWith("\t")) {
      if (inMechanics) flush();
      inMechanics = topKey[1] === "mechanics";
      continue;
    }
    if (!inMechanics) continue;
    const listStart = /^\s{2}-\s+name:\s*(.+)/.exec(line);
    if (listStart) { flush(); current = { name: scalar(listStart[1]) }; continue; }
    if (current) {
      const kv = /^\s{4}(\w[\w_]*):\s*(.+)/.exec(line);
      if (kv) {
        const k = kv[1];
        if (k === "description") current.description = scalar(kv[2]);
        else if (k === "intends") current.intends = scalar(kv[2]);
        else if (k === "intended_affect") current.intendedAffect = scalar(kv[2]);
        else if (k === "play_context") current.playContext = scalar(kv[2]);
      }
    }
  }
  flush();
  return mechanics;
}

// ── AND 条件フィルタ ──────────────────────────────────────────────────────────

function isPlayContextCompatible(
  candidate: MechanicMid,
  coreMechanics: MechanicMid[],
): boolean {
  // Phase 0: playContext が明示されていない場合は互換と見なす
  if (!candidate.playContext) return true;
  return coreMechanics.some(
    (m) => !m.playContext || m.playContext === candidate.playContext,
  );
}

// ── 公開 API ──────────────────────────────────────────────────────────────────

export async function combineMechanics(
  intake: GameIntake,
  evaluation: Step4Output,
  decomposition: Step5Output,
  profiles: PreferenceProfile[],
  gamesDir: string,
  restructureDir: string,
): Promise<Step6Output> {
  const others = await loadOtherGameMechanics(gamesDir, restructureDir, intake.slug);
  const genreProfile = findProfile(profiles, `genre:${intake.genre ?? ""}`);

  const coreMechanics = intake.mechanics.filter((m) =>
    decomposition.coreMechanics.includes(m.id),
  );
  const existingNames = new Set(intake.mechanics.map((m) => m.name.toLowerCase()));

  // ゲーム別に上位候補を選択 (spec §6: AND 条件)
  const gameGroups = new Map<string, OtherGameMechanic[]>();
  for (const other of others) {
    if (!gameGroups.has(other.slug)) gameGroups.set(other.slug, []);
    gameGroups.get(other.slug)!.push(other);
  }

  const candidates: CombinationCandidate[] = [];

  for (const [gameSlug, group] of gameGroups) {
    const topCandidates = group
      .filter((c) => {
        if (c.score < CANDIDATE_SCORE_THRESHOLD) return false;
        if (existingNames.has(c.mechanic.name.toLowerCase())) return false;
        if (!isPlayContextCompatible(c.mechanic, coreMechanics)) return false;
        return true;
      })
      .sort((a, b) => b.score - a.score)
      .slice(0, MAX_CANDIDATES_PER_GAME);

    for (const c of topCandidates) {
      // 嗜好プロファイルとの適合度
      const mechProfile = findProfile(profiles, `mechanic:${c.mechanic.name}`);
      const prefCompat = mechProfile && genreProfile
        ? cosine(mechProfile.vector, genreProfile.vector)
        : 0.5;

      if (prefCompat < PREFERENCE_COMPAT_THRESHOLD) continue;

      candidates.push({
        sourceGame: c.title,
        mechanic: c.mechanic,
        sourceScore: c.score,
        playContextCompatible: isPlayContextCompatible(c.mechanic, coreMechanics),
        preferenceCompatibility: prefCompat,
        rationale: `High-scoring mechanic (${c.score.toFixed(2)}) from ${c.title} with compatible play context and preference alignment.`,
        provenance: `${c.title} (slug: ${gameSlug})`,
      });
    }
  }

  // 嗜好適合 × 評価スコアで最終ランキング
  candidates.sort((a, b) => (b.sourceScore * b.preferenceCompatibility) - (a.sourceScore * a.preferenceCompatibility));

  return { candidates };
}
