/**
 * 人物パックの取込 (契約 v1 → flow_proxy_profile)。
 *
 * 入力は JSON 配列 / `{ "packs": [...] }` / JSONL のいずれか。1 件ずつ契約検証し、
 * 通らない行は理由付きで拒否して残りは取り込む (1 行の不備で全体を止めない)。
 *   - revoked=true の行は最小形 (contractVersion + speakerKey + genre + revoked) でも受け付けて失効させる
 *   - 同じ (speakerKey, genre) は内容 hash が同じなら何もしない (冪等)
 */

import { createHash } from "node:crypto";
import { maskedPersonaLabel } from "../../crawler/sources/persona.js";
import { parsePersonaPack, PROXY_CONTRACT_VERSION, SPEAKER_KEY_PATTERN, type PersonaPack } from "./contract.js";
import { revokeProxyProfile, upsertProxyProfile } from "./store.js";

export interface ProxyImportRejection {
  index: number;
  errors: string[];
}

export interface ProxyImportSummary {
  inserted: number;
  updated: number;
  unchanged: number;
  revoked: number;
  rejected: ProxyImportRejection[];
}

/** 露出名。speakerKey (仮名) をさらに一方向 hash する (既存の 論者#xxxxxx と同じ規則)。 */
export function proxyLabel(speakerKey: string): string {
  return maskedPersonaLabel(speakerKey);
}

/** 内容 hash (評価状態を戻すかの判定に使う)。キーの順序に依らないよう正規化して hash する。 */
export function packContentHash(pack: PersonaPack): string {
  const canonical = JSON.stringify(pack, (_k, v: unknown) =>
    v && typeof v === "object" && !Array.isArray(v)
      ? Object.fromEntries(Object.entries(v as Record<string, unknown>).sort(([a], [b]) => a.localeCompare(b)))
      : v
  );
  return createHash("sha256").update(canonical).digest("hex");
}

/** テキスト (JSON / JSONL) を生の行の配列にする。 */
export function parsePackInput(text: string): unknown[] {
  const trimmed = text.trim();
  if (!trimmed) return [];
  if (trimmed.startsWith("[") || trimmed.startsWith("{")) {
    try {
      const parsed = JSON.parse(trimmed) as unknown;
      if (Array.isArray(parsed)) return parsed;
      if (parsed && typeof parsed === "object" && Array.isArray((parsed as { packs?: unknown }).packs)) {
        return (parsed as { packs: unknown[] }).packs;
      }
      return [parsed];
    } catch {
      // 1 行 1 JSON (JSONL) として読み直す
    }
  }
  return trimmed
    .split(/\r?\n/)
    .map((line) => line.trim())
    .filter(Boolean)
    .map((line) => {
      try {
        return JSON.parse(line) as unknown;
      } catch {
        return { __invalidJson: line.slice(0, 40) };
      }
    });
}

function revokeNotice(raw: unknown): { speakerKey: string; genre: string } | null {
  if (!raw || typeof raw !== "object") return null;
  const o = raw as Record<string, unknown>;
  if (o.revoked !== true || o.contractVersion !== PROXY_CONTRACT_VERSION) return null;
  if (typeof o.speakerKey !== "string" || !SPEAKER_KEY_PATTERN.test(o.speakerKey)) return null;
  if (typeof o.genre !== "string" || !o.genre.trim()) return null;
  return { speakerKey: o.speakerKey, genre: o.genre.trim() };
}

/** 生の行の配列を取り込む。 */
export function importPersonaPacks(rows: readonly unknown[], now: number = Date.now()): ProxyImportSummary {
  const summary: ProxyImportSummary = { inserted: 0, updated: 0, unchanged: 0, revoked: 0, rejected: [] };
  rows.forEach((raw, index) => {
    const notice = revokeNotice(raw);
    if (notice) {
      if (revokeProxyProfile(notice.speakerKey, notice.genre, now)) summary.revoked++;
      else summary.unchanged++;
      return;
    }
    if (raw && typeof raw === "object" && "__invalidJson" in raw) {
      summary.rejected.push({ index, errors: ["JSON として読めない行"] });
      return;
    }
    const parsed = parsePersonaPack(raw);
    if (!parsed.ok) {
      summary.rejected.push({ index, errors: parsed.errors });
      return;
    }
    const result = upsertProxyProfile({
      pack: parsed.pack,
      label: proxyLabel(parsed.pack.speakerKey),
      contentHash: packContentHash(parsed.pack),
      now,
    });
    summary[result]++;
  });
  return summary;
}
