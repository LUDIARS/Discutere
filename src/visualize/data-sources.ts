/**
 * データソース・スナップショット (read-only)。
 *
 * KG (Kuzu/Core = better-sqlite3) の `sessions` / `utterances` を走査し、
 * 「学習データがどこから来たか = どの取得元 (source) が何件あるか」を集計する。
 * session.title は `<source>:<slug>` (ext 取込) / `discussion-of-gap:<gapId>` (自走議論) 等の
 * 規約なので、接頭辞を source として切り出す。
 *
 * 出力は learning-cache に焼かれ、学習ビューアの「データソース」タブが参照する
 * (KG を直接ロックしない)。集計は純粋関数なので Bot 稼働中でも安全。
 */

import type Database from "better-sqlite3";

/** source の素性 (表示ラベル / 種別 / 取得元の説明)。未知 source は import 扱いの汎用へ。 */
export type SourceKind = "import" | "internal" | "derived";

interface SourceMeta {
  label: string;
  kind: SourceKind;
  origin: string;
}

/** 既知 source のメタ。session.title の接頭辞 (`<source>:`) をキーにする。 */
const SOURCE_META: Record<string, SourceMeta> = {
  steam: { label: "Steam レビュー", kind: "import", origin: "store.steampowered.com appreviews API" },
  youtube: { label: "YouTube コメント", kind: "import", origin: "YouTube Data API v3" },
  niconico: { label: "ニコニコ動画 コメント", kind: "import", origin: "nicovideo snapshot + nvComment" },
  website: { label: "Web 記事", kind: "import", origin: "任意 URL の HTML 本文抽出" },
  fandom: { label: "Fandom / Wiki", kind: "import", origin: "MediaWiki action=parse" },
  reddit: { label: "Reddit", kind: "import", origin: "Reddit OAuth API" },
  opencritic: { label: "OpenCritic", kind: "import", origin: "OpenCritic (RapidAPI)" },
  "discussion-of-gap": { label: "自走議論 (facilitator/persona)", kind: "internal", origin: "Discatier 内部生成" },
  "discord-session": { label: "Discord 議論", kind: "internal", origin: "Discord guild" },
  reception: { label: "観測 affect 配線", kind: "derived", origin: "wire:affects (sentiment.json)" },
};

const FALLBACK_META: SourceMeta = { label: "(その他)", kind: "internal", origin: "不明" };

function metaFor(source: string): SourceMeta {
  return SOURCE_META[source] ?? { ...FALLBACK_META, label: source };
}

/** session.title から (source, slug) を切り出す。title 無しは internal 扱い。 */
function splitTitle(title: string | null, scene: string | null): { source: string; slug: string } {
  if (title) {
    const i = title.indexOf(":");
    if (i >= 0) return { source: title.slice(0, i), slug: title.slice(i + 1) };
    return { source: title, slug: "(none)" };
  }
  if (scene?.startsWith("discord:")) return { source: "discord-session", slug: "(discord)" };
  if (scene?.startsWith("gap:")) return { source: "discussion-of-gap", slug: "(gap)" };
  return { source: "(none)", slug: "(none)" };
}

export interface SourceEntry {
  source: string;
  label: string;
  kind: SourceKind;
  origin: string;
  sessions: number;
  utterances: number;
  games: string[]; // 取込対象スラッグ (gap uuid 等は除外、ゲーム取込のみ)
  gameCount: number;
  size: number; // グラフ用 = utterances
}

export interface GameSourceBreakdown {
  slug: string;
  sessions: number;
  utterances: number;
  sources: Array<{ source: string; label: string; kind: SourceKind; sessions: number; utterances: number }>;
}

export interface DataSourceSnapshot {
  workspaceId: string;
  generatedAt: number;
  totals: {
    sources: number;
    importSources: number;
    internalSources: number;
    utterances: number;
    importUtterances: number;
    games: number;
  };
  sources: SourceEntry[];
  byGame: GameSourceBreakdown[];
}

/** slug が「ゲーム取込」を指すか (gap uuid / (gap) / (discord) / (none) は除外)。 */
function isGameSlug(slug: string): boolean {
  if (!slug || slug.startsWith("(")) return false;
  // discussion-of-gap の slug は UUID。ハイフン4本の uuid 形は game ではない。
  if (/^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i.test(slug)) return false;
  return true;
}

/**
 * KG からデータソース構成を集計する。`db` は Core の raw better-sqlite3 ハンドル。
 */
export function buildDataSourceSnapshot(
  db: Database.Database,
  workspaceId: string,
  now: number = Date.now()
): DataSourceSnapshot {
  const sessions = db
    .prepare("SELECT id, title, scene FROM sessions WHERE workspace_id = ?")
    .all(workspaceId) as Array<{ id: string; title: string | null; scene: string | null }>;

  const uttRows = db
    .prepare(
      "SELECT session_id, COUNT(*) AS c FROM utterances WHERE workspace_id = ? GROUP BY session_id"
    )
    .all(workspaceId) as Array<{ session_id: string; c: number }>;
  const uttBySession = new Map(uttRows.map((r) => [r.session_id, r.c]));

  interface Acc {
    sessions: number;
    utterances: number;
    games: Set<string>;
  }
  const bySource = new Map<string, Acc>();
  const byGame = new Map<string, Map<string, { sessions: number; utterances: number }>>();

  for (const s of sessions) {
    const { source, slug } = splitTitle(s.title, s.scene);
    const utt = uttBySession.get(s.id) ?? 0;

    const acc = bySource.get(source) ?? { sessions: 0, utterances: 0, games: new Set<string>() };
    acc.sessions += 1;
    acc.utterances += utt;
    if (isGameSlug(slug)) acc.games.add(slug);
    bySource.set(source, acc);

    if (isGameSlug(slug)) {
      const g = byGame.get(slug) ?? new Map();
      const cur = g.get(source) ?? { sessions: 0, utterances: 0 };
      cur.sessions += 1;
      cur.utterances += utt;
      g.set(source, cur);
      byGame.set(slug, g);
    }
  }

  const sources: SourceEntry[] = [...bySource.entries()]
    .map(([source, acc]) => {
      const meta = metaFor(source);
      return {
        source,
        label: meta.label,
        kind: meta.kind,
        origin: meta.origin,
        sessions: acc.sessions,
        utterances: acc.utterances,
        games: [...acc.games].sort(),
        gameCount: acc.games.size,
        size: acc.utterances,
      };
    })
    .sort((a, b) => b.utterances - a.utterances);

  const gameBreakdowns: GameSourceBreakdown[] = [...byGame.entries()]
    .map(([slug, srcMap]) => {
      const srcs = [...srcMap.entries()]
        .map(([source, v]) => {
          const meta = metaFor(source);
          return { source, label: meta.label, kind: meta.kind, sessions: v.sessions, utterances: v.utterances };
        })
        .sort((a, b) => b.utterances - a.utterances);
      return {
        slug,
        sessions: srcs.reduce((n, x) => n + x.sessions, 0),
        utterances: srcs.reduce((n, x) => n + x.utterances, 0),
        sources: srcs,
      };
    })
    .sort((a, b) => b.utterances - a.utterances);

  const importSources = sources.filter((s) => s.kind === "import");
  return {
    workspaceId,
    generatedAt: now,
    totals: {
      sources: sources.length,
      importSources: importSources.length,
      internalSources: sources.length - importSources.length,
      utterances: sources.reduce((n, s) => n + s.utterances, 0),
      importUtterances: importSources.reduce((n, s) => n + s.utterances, 0),
      games: byGame.size,
    },
    sources,
    byGame: gameBreakdowns,
  };
}
