/**
 * データソース隔離 (quarantine) — 取得元単位での退避と復元。
 *
 * 「この取得元のデータは議論・学習に使いたくない」と判断したとき、その source
 * (任意で slug 指定) に属する session とその配下 (utterance / reaction / 採点 / embedding 等) を
 * KG (discatier.kuzu) から **別 SQLite ファイルへ丸ごと退避してから削除** する。
 * 退避ファイルがあれば後で完全復元できる (=可逆な隔離)。
 *
 * 退避は JSON ではなく ATTACH した SQLite に `INSERT ... SELECT` で写すため、
 * steam レビュー (数十万 utterance) のような大規模ソースでもメモリを食わずに動く。
 *
 * KG は単一 writer。Bot / learning-viewer (live) を止めてから実行すること。
 */

import fs from "node:fs";
import path from "node:path";

import Database from "better-sqlite3";

import { DEFAULT_INGESTED_PATH } from "./sources/ingested-store.js";

/** KG 上で session/utterance を参照する子テーブル群。 */
const SESSION_KEYED = ["sessions", "utterances", "affects"] as const; // id / session_id
const UTTERANCE_KEYED = ["reactions", "translation_proposals"] as const; // utterance_id
const NODE_KEYED = ["embeddings", "opinion_scores", "discord_message_map"] as const; // node_id / target_id
const ARCHIVE_TABLES = [...SESSION_KEYED, ...UTTERANCE_KEYED, ...NODE_KEYED] as const;

type TableName = (typeof ARCHIVE_TABLES)[number];

/** node_id / target_id を持つテーブルの参照カラム名。 */
const NODE_COL: Record<(typeof NODE_KEYED)[number], string> = {
  embeddings: "node_id",
  opinion_scores: "target_id",
  discord_message_map: "target_id",
};

export interface QuarantineCounts {
  sessions: number;
  utterances: number;
  reactions: number;
  translation_proposals: number;
  embeddings: number;
  opinion_scores: number;
  discord_message_map: number;
  affects: number;
  ingested: number;
}

export interface QuarantinePlan {
  source: string;
  slug: string | null;
  sessionIds: string[];
  counts: QuarantineCounts;
}

function kuzuPath(): string {
  return process.env.DISCATIER_KUZU_PATH ?? path.resolve("./data/discatier.kuzu");
}

/** session.title (`<source>:<slug>`) から source/slug を切り出す。 */
function splitTitle(title: string | null): { source: string; slug: string } {
  if (!title) return { source: "(none)", slug: "(none)" };
  const i = title.indexOf(":");
  if (i < 0) return { source: title, slug: "(none)" };
  return { source: title.slice(0, i), slug: title.slice(i + 1) };
}

/** id 集合を SQL の IN リスト用にクオートする (UUID 等。シングルクオートはエスケープ)。 */
function inList(ids: string[]): string {
  return ids.map((id) => `'${id.replace(/'/g, "''")}'`).join(",");
}

/** 対象 session を解決して件数を見積もる (dry-run プレビュー)。 */
export function planQuarantine(
  db: Database.Database,
  workspaceId: string,
  source: string,
  slug: string | null
): QuarantinePlan {
  const sessions = db
    .prepare("SELECT id, title FROM sessions WHERE workspace_id = ?")
    .all(workspaceId) as Array<{ id: string; title: string | null }>;
  const targets = sessions.filter((s) => {
    const sp = splitTitle(s.title);
    return sp.source === source && (slug === null || sp.slug === slug);
  });
  const sessionIds = targets.map((s) => s.id);
  const counts = emptyCounts();
  if (sessionIds.length === 0) return { source, slug, sessionIds, counts };

  const S = inList(sessionIds);
  const Usub = `SELECT id FROM utterances WHERE session_id IN (${S})`;
  const count = (sql: string): number =>
    (db.prepare(sql).get() as { c: number }).c;
  counts.sessions = sessionIds.length;
  counts.utterances = count(`SELECT COUNT(*) c FROM utterances WHERE session_id IN (${S})`);
  counts.affects = count(`SELECT COUNT(*) c FROM affects WHERE session_id IN (${S})`);
  counts.reactions = count(`SELECT COUNT(*) c FROM reactions WHERE utterance_id IN (${Usub})`);
  counts.translation_proposals = count(
    `SELECT COUNT(*) c FROM translation_proposals WHERE utterance_id IN (${Usub})`
  );
  for (const t of NODE_KEYED) {
    const col = NODE_COL[t];
    counts[t] = count(
      `SELECT COUNT(*) c FROM ${t} WHERE ${col} IN (${Usub}) OR ${col} IN (${S})`
    );
  }
  return { source, slug, sessionIds, counts };
}

function emptyCounts(): QuarantineCounts {
  return {
    sessions: 0,
    utterances: 0,
    reactions: 0,
    translation_proposals: 0,
    embeddings: 0,
    opinion_scores: 0,
    discord_message_map: 0,
    affects: 0,
    ingested: 0,
  };
}

/** 各テーブルの「対象行」を選ぶ WHERE 句 (session id 集合 S を埋め込む)。 */
function whereFor(table: TableName, S: string, Usub: string): string {
  if (table === "sessions") return `id IN (${S})`;
  if (table === "utterances" || table === "affects") return `session_id IN (${S})`;
  if (table === "reactions" || table === "translation_proposals")
    return `utterance_id IN (${Usub})`;
  const col = NODE_COL[table as (typeof NODE_KEYED)[number]];
  return `${col} IN (${Usub}) OR ${col} IN (${S})`;
}

export interface QuarantineResult {
  archiveFile: string;
  counts: QuarantineCounts;
}

/**
 * source (任意で slug) を KG から退避ファイルへ写し、KG から削除する。
 * 退避先 = `<archiveDir>/<source>[-<slug>]-<ts>.sqlite`。slug 未指定なら source 全体。
 */
export function quarantineSource(
  db: Database.Database,
  workspaceId: string,
  source: string,
  slug: string | null,
  opts: { archiveDir?: string; ingestedPath?: string; now?: number } = {}
): QuarantineResult {
  const archiveDir = opts.archiveDir ?? path.resolve("./data/quarantine");
  const now = opts.now ?? Date.now();
  const plan = planQuarantine(db, workspaceId, source, slug);
  if (plan.sessionIds.length === 0) {
    throw new Error(`隔離対象の session が見つかりません: source=${source} slug=${slug ?? "(all)"}`);
  }

  fs.mkdirSync(archiveDir, { recursive: true });
  const stamp = new Date(now).toISOString().replace(/[:.]/g, "-");
  const name = `${source}${slug ? "-" + slug : ""}-${stamp}.sqlite`;
  const archiveFile = path.join(archiveDir, name);
  if (fs.existsSync(archiveFile)) fs.rmSync(archiveFile);

  const S = inList(plan.sessionIds);
  const Usub = `SELECT id FROM utterances WHERE session_id IN (${S})`;
  const archAbs = archiveFile.replace(/'/g, "''");

  db.exec(`ATTACH DATABASE '${archAbs}' AS arc`);
  try {
    db.exec("BEGIN");
    // メタ
    db.exec(
      "CREATE TABLE arc._quarantine_meta (source TEXT, slug TEXT, workspace_id TEXT, created_at INTEGER, kuzu_path TEXT)"
    );
    db.prepare(
      "INSERT INTO arc._quarantine_meta VALUES (?, ?, ?, ?, ?)"
    ).run(source, slug, workspaceId, now, kuzuPath());

    // 退避 (SELECT を arc へ複製)。削除前に全テーブルを写す。
    for (const t of ARCHIVE_TABLES) {
      db.exec(`CREATE TABLE arc.${t} AS SELECT * FROM main.${t} WHERE ${whereFor(t, S, Usub)}`);
    }

    // 削除は依存順: utterance 参照 → utterances → session 参照 → sessions。
    for (const t of UTTERANCE_KEYED) db.exec(`DELETE FROM main.${t} WHERE ${whereFor(t, S, Usub)}`);
    for (const t of NODE_KEYED) db.exec(`DELETE FROM main.${t} WHERE ${whereFor(t, S, Usub)}`);
    db.exec(`DELETE FROM main.utterances WHERE session_id IN (${S})`);
    db.exec(`DELETE FROM main.affects WHERE session_id IN (${S})`);
    db.exec(`DELETE FROM main.sessions WHERE id IN (${S})`);

    db.exec("COMMIT");
  } catch (e) {
    db.exec("ROLLBACK");
    db.exec("DETACH DATABASE arc");
    if (fs.existsSync(archiveFile)) fs.rmSync(archiveFile);
    throw e;
  }
  db.exec("DETACH DATABASE arc");

  const counts: QuarantineCounts = { ...plan.counts };

  // ingested-store: source 全体の隔離時のみ dedup 台帳も退避+削除 (slug 単位は不可)。
  if (slug === null) {
    const ingestedPath = opts.ingestedPath ?? DEFAULT_INGESTED_PATH;
    if (fs.existsSync(ingestedPath)) {
      const idb = new Database(ingestedPath);
      try {
        idb.exec(`ATTACH DATABASE '${archAbs}' AS arc`);
        const n = (idb.prepare("SELECT COUNT(*) c FROM ingested WHERE source = ?").get(source) as { c: number }).c;
        idb.exec("BEGIN");
        idb.exec(
          `CREATE TABLE arc.ingested AS SELECT * FROM main.ingested WHERE source = '${source.replace(/'/g, "''")}'`
        );
        idb.prepare("DELETE FROM ingested WHERE source = ?").run(source);
        idb.exec("COMMIT");
        idb.exec("DETACH DATABASE arc");
        counts.ingested = n;
      } finally {
        idb.close();
      }
    }
  }

  writeManifest(archiveFile, { source, slug, workspaceId, createdAt: now, counts });
  return { archiveFile, counts };
}

/** 退避ファイルから KG (と ingested-store) へ全行を戻す。 */
export function restoreQuarantine(
  db: Database.Database,
  archiveFile: string,
  opts: { ingestedPath?: string } = {}
): { counts: QuarantineCounts } {
  if (!fs.existsSync(archiveFile)) throw new Error(`退避ファイルが見つかりません: ${archiveFile}`);
  const archAbs = path.resolve(archiveFile).replace(/'/g, "''");
  const counts = emptyCounts();

  db.exec(`ATTACH DATABASE '${archAbs}' AS arc`);
  try {
    const archTables = new Set(
      (db.prepare("SELECT name FROM arc.sqlite_master WHERE type='table'").all() as Array<{ name: string }>).map(
        (r) => r.name
      )
    );
    db.exec("BEGIN");
    for (const t of ARCHIVE_TABLES) {
      if (!archTables.has(t)) continue;
      const before = (db.prepare(`SELECT COUNT(*) c FROM main.${t}`).get() as { c: number }).c;
      db.exec(`INSERT OR IGNORE INTO main.${t} SELECT * FROM arc.${t}`);
      const after = (db.prepare(`SELECT COUNT(*) c FROM main.${t}`).get() as { c: number }).c;
      (counts as unknown as Record<string, number>)[t] = after - before;
    }
    db.exec("COMMIT");
  } catch (e) {
    db.exec("ROLLBACK");
    db.exec("DETACH DATABASE arc");
    throw e;
  }
  db.exec("DETACH DATABASE arc");

  // ingested-store 復元 (arc.ingested があれば)。
  const ingestedPath = opts.ingestedPath ?? DEFAULT_INGESTED_PATH;
  const idb = new Database(ingestedPath);
  try {
    idb.exec(`ATTACH DATABASE '${archAbs}' AS arc`);
    const has = idb
      .prepare("SELECT 1 FROM arc.sqlite_master WHERE type='table' AND name='ingested'")
      .get();
    if (has) {
      idb.exec(
        "CREATE TABLE IF NOT EXISTS ingested (source TEXT NOT NULL, native_id TEXT NOT NULL, imported_at INTEGER NOT NULL, PRIMARY KEY (source, native_id))"
      );
      const before = (idb.prepare("SELECT COUNT(*) c FROM ingested").get() as { c: number }).c;
      idb.exec("INSERT OR IGNORE INTO ingested SELECT * FROM arc.ingested");
      const after = (idb.prepare("SELECT COUNT(*) c FROM ingested").get() as { c: number }).c;
      counts.ingested = after - before;
    }
    idb.exec("DETACH DATABASE arc");
  } finally {
    idb.close();
  }
  return { counts };
}

export interface ArchiveInfo {
  file: string;
  source: string;
  slug: string | null;
  createdAt: number;
  counts: QuarantineCounts;
}

/** 退避ディレクトリの隔離アーカイブ一覧 (manifest sidecar から)。 */
export function listArchives(archiveDir: string = path.resolve("./data/quarantine")): ArchiveInfo[] {
  if (!fs.existsSync(archiveDir)) return [];
  return fs
    .readdirSync(archiveDir)
    .filter((f) => f.endsWith(".sqlite"))
    .map((f) => {
      const file = path.join(archiveDir, f);
      const manifest = file.replace(/\.sqlite$/, ".json");
      if (fs.existsSync(manifest)) {
        try {
          const m = JSON.parse(fs.readFileSync(manifest, "utf8"));
          return { file, source: m.source, slug: m.slug ?? null, createdAt: m.createdAt, counts: m.counts } as ArchiveInfo;
        } catch {
          /* fall through */
        }
      }
      return { file, source: "?", slug: null, createdAt: 0, counts: emptyCounts() };
    })
    .sort((a, b) => b.createdAt - a.createdAt);
}

function writeManifest(
  archiveFile: string,
  data: { source: string; slug: string | null; workspaceId: string; createdAt: number; counts: QuarantineCounts }
): void {
  const manifest = archiveFile.replace(/\.sqlite$/, ".json");
  fs.writeFileSync(manifest, JSON.stringify(data, null, 2));
}
