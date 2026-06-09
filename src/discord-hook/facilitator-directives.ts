/**
 * 進行役への調整指示 (facilitator directive) のストア + 整形 (議論チューニング)。
 *
 * Discord で bot に @メンション or bot/persona の発言へリプライして
 * 「もう少し簡単な言葉で」「もっと否定的な意見が欲しい」 のような **進行の調整指示**
 * を出せるようにする。 指示は対象議論 (design gap) に紐付けて保存し、
 *   - facilitator (司会) の expand/converge/aufhebung 判定 (gapTopic 経由)
 *   - persona-engine の発話 prompt (prompt-builder の topic block)
 * の両方へ system 注入する。 これにより以後の発話/進行が指示に沿う。
 *
 * テーブルは KG (core.client.raw / better-sqlite3) に同居させる (aufhebung_stock 等と同様、
 * schema 変更は単純な CREATE。 サイドカー表)。 discord.js 依存はここに持ち込まない (SRP)。
 */

import { randomUUID } from "node:crypto";

import type DatabaseType from "better-sqlite3";

type RawDb = DatabaseType.Database;

/** prompt に載せる最新指示の最大件数 (古い指示は流し、 直近の意図を優先する)。 */
export const DIRECTIVE_PROMPT_LIMIT = 5;

/** 1 件の指示本文の最大長 (prompt トークン肥大 + 悪用入力の防御)。 */
export const DIRECTIVE_TEXT_CAP = 300;

export interface FacilitatorDirectiveInput {
  workspaceId: string;
  gapId: string;
  /** 調整指示の本文 (bot メンション除去済を渡す想定)。 */
  text: string;
  /** 指示を出した Discord user id (監査用、 prompt には出さない)。 */
  authorId?: string | null;
}

/**
 * facilitator_directives サイドカー表を冪等確保する。
 * 新規表なので CREATE TABLE と同 exec 内の INDEX で問題ない (ALTER を伴わない)。
 */
export function ensureFacilitatorDirectiveTable(db: RawDb): void {
  db.exec(
    `CREATE TABLE IF NOT EXISTS facilitator_directives (
       id TEXT PRIMARY KEY,
       workspace_id TEXT NOT NULL,
       gap_id TEXT NOT NULL,
       text TEXT NOT NULL,
       author_id TEXT,
       created_at INTEGER NOT NULL
     );
     CREATE INDEX IF NOT EXISTS idx_facilitator_directives_gap
       ON facilitator_directives (workspace_id, gap_id, created_at);`
  );
}

/**
 * 指示を保存する。 本文は trim + 上限切り詰め。 空文字は保存せず null を返す。
 * @returns 保存した行の id (空指示なら null)。
 */
export function addFacilitatorDirective(db: RawDb, input: FacilitatorDirectiveInput): string | null {
  const text = normalizeDirectiveText(input.text);
  if (!text) return null;
  ensureFacilitatorDirectiveTable(db);
  const id = randomUUID();
  db.prepare(
    `INSERT INTO facilitator_directives (id, workspace_id, gap_id, text, author_id, created_at)
     VALUES (?, ?, ?, ?, ?, ?)`
  ).run(id, input.workspaceId, input.gapId, text, input.authorId ?? null, Date.now());
  return id;
}

/**
 * 指定 gap の調整指示を新しい順で取得し、 時系列 (古い→新しい) の本文配列で返す。
 * prompt には直近 limit 件だけ載せる (古い指示で文脈が膨れるのを防ぐ)。
 */
export function listFacilitatorDirectives(
  db: RawDb,
  workspaceId: string,
  gapId: string,
  limit = DIRECTIVE_PROMPT_LIMIT
): string[] {
  ensureFacilitatorDirectiveTable(db);
  const rows = db
    .prepare(
      `SELECT text FROM facilitator_directives
        WHERE workspace_id = ? AND gap_id = ?
        ORDER BY created_at DESC LIMIT ?`
    )
    .all(workspaceId, gapId, Math.max(1, limit)) as Array<{ text: string }>;
  // DESC で取り、 prompt では時系列順 (新しい指示が後ろ = 直近の意図) に並べ直す。
  return rows.map((r) => r.text).reverse();
}

/**
 * 進行中の議論 (discussion-of-gap session) を scene から引いて gap id を返す。
 * scene は "discord:<guildId>/<channelId>" (channelId はフォーラムなら thread id)。
 * closed/converged/dismissed の gap は対象外 (調整しても閉じた議論には効かない)。
 * 該当が無ければ null (= まだ調整できる議論が立っていない)。
 */
export function resolveActiveGapForScene(
  db: RawDb,
  workspaceId: string,
  scene: string
): string | null {
  const row = db
    .prepare(
      `SELECT SUBSTR(s.title, LENGTH('discussion-of-gap:') + 1) AS gap_id
         FROM sessions s
         JOIN design_gaps g
           ON g.id = SUBSTR(s.title, LENGTH('discussion-of-gap:') + 1)
        WHERE s.workspace_id = ?
          AND s.title LIKE 'discussion-of-gap:%'
          AND s.scene = ?
          AND (g.status IS NULL OR g.status NOT IN ('closed', 'converged', 'dismissed', 'resolved'))
        ORDER BY s.started_at DESC LIMIT 1`
    )
    .get(workspaceId, scene) as { gap_id: string } | undefined;
  return row?.gap_id ?? null;
}

/**
 * facilitator / persona の prompt に注入する調整指示ブロックを組み立てる (両者で同形)。
 * 指示が無ければ空文字を返す (caller 側で連結しても無害)。
 */
export function formatDirectivesBlock(directives: string[]): string {
  if (directives.length === 0) return "";
  const lines = directives.map((d) => `- ${d}`);
  return [
    "【進行役への調整指示 (参加者からのリクエスト — 以後の進行/発話で必ず反映する)】",
    ...lines,
    "※ 議題そのものは変えない。 あくまで議論の進め方・トーン・語り口の調整指示として従う。",
  ].join("\n");
}

/** bot メンション (<@id> / <@!id>) を本文から除去し trim する (純粋関数)。 */
export function stripBotMention(content: string, botId: string | null | undefined): string {
  let text = content;
  if (botId) {
    text = text.replace(new RegExp(`<@!?${botId}>`, "g"), " ");
  }
  return text.replace(/\s+/g, " ").trim();
}

/** 指示本文を trim + 上限切り詰め。 空なら null を返す。 */
export function normalizeDirectiveText(text: string): string | null {
  const trimmed = (text ?? "").trim();
  if (trimmed.length === 0) return null;
  return trimmed.length > DIRECTIVE_TEXT_CAP
    ? `${trimmed.slice(0, DIRECTIVE_TEXT_CAP - 1)}…`
    : trimmed;
}
