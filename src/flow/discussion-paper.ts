/**
 * ディスカッションペーパー生成 (OVERVIEW §5, discussion.md step 3)。
 *
 * ロール × LLM 性質で配布内容が変わる:
 *   - 賛成派/反対派 (debater, クラウド LLM): 共通項目のみ
 *   - 意見屋 (opinion): + ユーザ意見まとめ
 *   - ローカル LLM (isLocal=true): ロール問わず + RAG ユーザ意見
 *   - 機密タグ: ユーザ意見の代わりに (synthetic) ラベル付き想定意見
 */

import { randomUUID } from "node:crypto";
import type { FlowTag } from "./tags.js";
import { paperSupplement } from "./tags.js";
import type { FlowPersona, FlowStance } from "./personas.js";
import type { MechanicSummary } from "./investigate.js";
import { getFlowDb } from "./db/connection.js";

export interface ContextVoice {
  content: string;
  source: string;
  sourceUrl?: string;
}

export interface RoundSummary {
  round: number;
  summary: string;
  aufhebung: string[];
}

export interface DiscussionPaper {
  paperId: string;
  sessionId: string;
  theme: string;
  tags: FlowTag[];
  mechanics: MechanicSummary[];
  supplement: string;
  rounds: RoundSummary[];
  /**
   * 議論ブリーフ本文の正本 markdown (ハイブリッド源泉モデル, paper-markdown.ts)。
   * 指定時は buildPaperSystem がこれをそのまま system に載せる (各 LLM が md を直接参照)。
   * 未指定の旧経路は構造化フィールドから従来どおり組み立てる (後方互換)。
   */
  bodyMd?: string;
}

/** ペルソナへの配布内容 (ターンごとに組み立て) */
export interface PersonaPaper {
  theme: string;
  supplement: string;
  mechanicsText: string;
  previousRoundsText: string;
  currentRoundUtterances: string[];
  userOpinionsText?: string;
  /** 議論ブリーフ本文の正本 markdown (あれば buildPaperSystem がこれを優先する)。 */
  bodyMd?: string;
}

export interface BuildPersonaPaperArgs {
  paper: DiscussionPaper;
  persona: FlowPersona;
  stance: FlowStance;
  currentRoundUtterances: Array<{ personaName: string; text: string }>;
  userVoices: ContextVoice[];
  syntheticOpinions?: string[];
}

/** 機密タグ時の想定意見データを生成する (mechanics.intended_affect から簡易合成)。 */
export function synthesizeOpinions(mechanics: MechanicSummary[]): string[] {
  return mechanics
    .filter((m) => m.intended_affect)
    .map(
      (m) =>
        `[想定 (synthetic)] 「${m.name}」について: ${m.intended_affect} という体験を期待している意見が考えられます。`
    );
}

/** ユーザ意見テキストを構築する (RAG 用)。件数の上限は呼び出し側 (director の lookup 件数) で決まる。 */
function buildUserOpinionsText(voices: ContextVoice[]): string {
  if (voices.length === 0) return "";
  const lines = voices
    .slice(0, 20)
    .map(
      (v, i) => `${i + 1}. ${v.content.slice(0, 200)}${v.content.length > 200 ? "…" : ""}（出所: ${v.source}）`
    );
  return `## ユーザの声 (参考)\n${lines.join("\n")}`;
}

/** 想定意見テキストを構築する (機密テーマ用)。 */
function buildSyntheticOpinionsText(opinions: string[]): string {
  if (opinions.length === 0) return "";
  return `## 想定意見データ (synthetic / 実ユーザデータなし)\n${opinions.join("\n")}`;
}

/**
 * メカニクスリストを人間が読みやすい形式に変換する。
 */
function formatMechanics(mechanics: MechanicSummary[]): string {
  if (mechanics.length === 0) return "(メカニクスデータなし)";
  return mechanics
    .map((m) => {
      const affect = m.intended_affect ? ` → 期待感情: ${m.intended_affect}` : "";
      return `- **${m.name}**: ${m.description}${affect}`;
    })
    .join("\n");
}

/** 前ラウンドサマリをテキスト化する。 */
function formatPreviousRounds(rounds: RoundSummary[]): string {
  if (rounds.length === 0) return "(前ラウンドなし)";
  return rounds
    .map((r) => {
      const auf = r.aufhebung.length > 0 ? `\n  止揚: ${r.aufhebung.join(" / ")}` : "";
      return `### ラウンド ${r.round} まとめ\n${r.summary}${auf}`;
    })
    .join("\n\n");
}

/**
 * ペルソナ用ディスカッションペーパーを組み立てる (ターンごとに呼ぶ)。
 *
 * ロール別配布 (OVERVIEW §5.2):
 *   - debater (クラウド LLM): 共通項目のみ
 *   - opinion: + ユーザ意見
 *   - isLocal: ロール問わず + ユーザ意見 (§5.3)
 *   - 機密タグ: ユーザ意見の代わりに synthetic (§5.4)
 */
export function buildPersonaPaper(args: BuildPersonaPaperArgs): PersonaPaper {
  const { paper, persona, currentRoundUtterances, userVoices, syntheticOpinions = [] } = args;
  const isConfidential = paper.tags.includes("機密");

  const mechanicsText = formatMechanics(paper.mechanics);
  const previousRoundsText = formatPreviousRounds(paper.rounds);
  const supplement = paperSupplement(paper.tags);

  let userOpinionsText: string | undefined;

  const wantsOpinions = persona.role === "opinion" || persona.isLocal;
  if (wantsOpinions) {
    if (isConfidential) {
      // 機密: 想定意見データ (synthetic)
      const synthetic = syntheticOpinions.length > 0 ? syntheticOpinions : synthesizeOpinions(paper.mechanics);
      userOpinionsText = buildSyntheticOpinionsText(synthetic);
    } else {
      userOpinionsText = buildUserOpinionsText(userVoices);
    }
  }

  return {
    theme: paper.theme,
    supplement,
    mechanicsText,
    previousRoundsText,
    currentRoundUtterances: currentRoundUtterances.map(
      (u) => `${u.personaName}: ${u.text}`
    ),
    userOpinionsText,
    bodyMd: paper.bodyMd,
  };
}

/**
 * セッション不変の安定部 (議題 + 観点補足 + メカニクス) を返す。
 * 全ペルソナ・全ターンで同一バイトになるため SDK の cache_control(system) で再利用できる (E)。
 */
export function buildPaperSystem(p: PersonaPaper): string {
  // 正本 markdown があればそれをそのまま system に載せる (各 LLM が md を直接参照)。
  if (p.bodyMd && p.bodyMd.trim()) return p.bodyMd;
  return [
    `# 議題\n${p.theme}`,
    p.supplement ? `# 観点補足\n${p.supplement}` : null,
    `# ゲームのメカニクス\n${p.mechanicsText}`,
  ]
    .filter(Boolean)
    .join("\n\n");
}

/**
 * ペルソナ固有 + 可変部 (前ラウンド結果 / 当ラウンド意見 / ユーザの声) を返す。
 * 安定部 (buildPaperSystem) は含めない (= user メッセージ側)。
 */
export function buildPersonaUserPrompt(p: PersonaPaper, stance: FlowStance, persona: FlowPersona): string {
  const stanceLine =
    stance === "neutral"
      ? "あなたはファシリテーター。対立を整理し議論を前に進める。"
      : stance === "pro"
        ? "このターンのあなたの立場は【賛成寄り】。主張を筋の通った形で擁護・補強する。"
        : stance === "con"
          ? "このターンのあなたの立場は【反対寄り】。健全な反論・反例・見落とされた弱点を投げる。"
          : "あなたは意見役。ユーザの声を踏まえつつ自分の角度で意見を述べる。";

  const volatile = [
    p.previousRoundsText !== "(前ラウンドなし)" ? `# 前ラウンドの結果\n${p.previousRoundsText}` : null,
    p.currentRoundUtterances.length > 0
      ? `# 当ラウンドの意見 (これまで)\n${p.currentRoundUtterances.join("\n")}`
      : null,
    p.userOpinionsText ? p.userOpinionsText : null,
  ].filter(Boolean);

  // 憑依 (B, item4): データ由来の人物像を演じている場合、その感性を prompt に注入する。
  const possessionLine = persona.possession
    ? `あなたは今、データ由来の人物像「${persona.possession.label}」を憑依している。${
        persona.possession.descriptor ?? ""
      }`
    : null;

  return [
    `あなたは議論ペルソナ「${persona.name}」。`,
    `特徴: ${persona.traits.join(" / ")} / 話し方: ${persona.speechStyle}`,
    ...(possessionLine ? [possessionLine] : []),
    stanceLine,
    "Discord のチャットで実在の人間が話すように、自然な口語で 1〜2 文だけ書く。",
    "ラベルや箇条書きは使わない。既出の繰り返しは避け、議論を一歩進める。",
    "発言テキストのみを返す (JSON や前置きは不要)。付け足す事が無ければ空行のみ返す。",
    ...(volatile.length ? ["", "---", ...volatile] : []),
  ].join("\n");
}

/**
 * PersonaPaper を LLM に渡すプロンプト文字列に変換する (単一文字列・後方互換)。
 * SDK キャッシュ経路は buildPaperSystem(system) + buildPersonaUserPrompt(user) に分けて使う。
 */
export function paperToPrompt(p: PersonaPaper, stance: FlowStance, persona: FlowPersona): string {
  const stanceLine =
    stance === "neutral"
      ? "あなたはファシリテーター。対立を整理し議論を前に進める。"
      : stance === "pro"
        ? "このターンのあなたの立場は【賛成寄り】。主張を筋の通った形で擁護・補強する。"
        : stance === "con"
          ? "このターンのあなたの立場は【反対寄り】。健全な反論・反例・見落とされた弱点を投げる。"
          : "あなたは意見役。ユーザの声を踏まえつつ自分の角度で意見を述べる。";

  const sections = [
    `# 議題\n${p.theme}`,
    p.supplement ? `# 観点補足\n${p.supplement}` : null,
    `# ゲームのメカニクス\n${p.mechanicsText}`,
    p.previousRoundsText !== "(前ラウンドなし)"
      ? `# 前ラウンドの結果\n${p.previousRoundsText}`
      : null,
    p.currentRoundUtterances.length > 0
      ? `# 当ラウンドの意見 (これまで)\n${p.currentRoundUtterances.join("\n")}`
      : null,
    p.userOpinionsText ? p.userOpinionsText : null,
  ].filter(Boolean);

  return [
    `あなたは議論ペルソナ「${persona.name}」。`,
    `特徴: ${persona.traits.join(" / ")} / 話し方: ${persona.speechStyle}`,
    stanceLine,
    "Discord のチャットで実在の人間が話すように、自然な口語で 1〜2 文だけ書く。",
    "ラベルや箇条書きは使わない。既出の繰り返しは避け、議論を一歩進める。",
    "発言テキストのみを返す (JSON や前置きは不要)。付け足す事が無ければ空行のみ返す。",
    "",
    "---",
    ...sections,
  ].join("\n");
}

// ── 永続化 ──────────────────────────────────────────────────────────────────

/**
 * discussion_paper を DB に保存し paperId を返す。
 * @param flow フロー種別 ("discussion" / "improvement" 等)。既定 "discussion"。
 */
export function persistPaper(
  paper: Omit<DiscussionPaper, "paperId" | "rounds">,
  flow = "discussion"
): string {
  const db = getFlowDb();
  const now = Date.now();
  // 同 session の行 (= 編集ゲートで作った draft) があれば 'started' に upsert (重複行を作らない)。
  const existing = db
    .prepare(`SELECT id FROM discussion_paper WHERE session_id = ?`)
    .get(paper.sessionId) as { id: string } | undefined;
  if (existing) {
    db.prepare(
      `UPDATE discussion_paper
          SET flow = ?, theme = ?, tags_json = ?, mechanics_json = ?, supplement = ?, body_md = ?,
              status = 'started', updated_at = ?
        WHERE id = ?`
    ).run(
      flow,
      paper.theme,
      JSON.stringify(paper.tags),
      JSON.stringify(paper.mechanics),
      paper.supplement,
      paper.bodyMd ?? null,
      now,
      existing.id
    );
    return existing.id;
  }
  const paperId = randomUUID();
  db.prepare(
    `INSERT INTO discussion_paper (id, flow, session_id, theme, tags_json, mechanics_json, supplement, body_md, status, created_at, updated_at)
     VALUES (?, ?, ?, ?, ?, ?, ?, ?, 'started', ?, ?)`
  ).run(
    paperId,
    flow,
    paper.sessionId,
    paper.theme,
    JSON.stringify(paper.tags),
    JSON.stringify(paper.mechanics),
    paper.supplement,
    paper.bodyMd ?? null,
    now,
    now
  );
  return paperId;
}

/** 編集ゲートのドラフト行 (status='draft') の復元用スナップショット。 */
export interface DraftPaperRow {
  paperId: string;
  flow: string;
  theme: string;
  tags: FlowTag[];
  mechanics: MechanicSummary[];
  supplement: string;
  bodyMd: string;
}

/**
 * 編集中ドラフトを discussion_paper に status='draft' で永続 (議論一覧に「下書き」として出す)。
 * 同 session に行があれば内容を更新する (status は変えない=既に started なら上書きしない)。
 */
export function persistDraftPaper(
  paper: Omit<DiscussionPaper, "paperId" | "rounds">,
  flow = "discussion"
): string {
  const db = getFlowDb();
  const now = Date.now();
  const existing = db
    .prepare(`SELECT id FROM discussion_paper WHERE session_id = ?`)
    .get(paper.sessionId) as { id: string } | undefined;
  if (existing) {
    db.prepare(
      `UPDATE discussion_paper
          SET flow = ?, theme = ?, tags_json = ?, mechanics_json = ?, supplement = ?, body_md = ?, updated_at = ?
        WHERE id = ?`
    ).run(
      flow,
      paper.theme,
      JSON.stringify(paper.tags),
      JSON.stringify(paper.mechanics),
      paper.supplement,
      paper.bodyMd ?? null,
      now,
      existing.id
    );
    return existing.id;
  }
  const paperId = randomUUID();
  db.prepare(
    `INSERT INTO discussion_paper (id, flow, session_id, theme, tags_json, mechanics_json, supplement, body_md, status, created_at, updated_at)
     VALUES (?, ?, ?, ?, ?, ?, ?, ?, 'draft', ?, ?)`
  ).run(
    paperId,
    flow,
    paper.sessionId,
    paper.theme,
    JSON.stringify(paper.tags),
    JSON.stringify(paper.mechanics),
    paper.supplement,
    paper.bodyMd ?? null,
    now,
    now
  );
  return paperId;
}

/** session の draft ペーパー行を返す (status='draft' のみ。無ければ null)。編集の再開に使う。 */
export function getDraftPaper(sessionId: string): DraftPaperRow | null {
  const row = getFlowDb()
    .prepare(
      `SELECT id, flow, theme, tags_json, mechanics_json, supplement, body_md
         FROM discussion_paper WHERE session_id = ? AND status = 'draft'`
    )
    .get(sessionId) as
    | {
        id: string;
        flow: string;
        theme: string;
        tags_json: string;
        mechanics_json: string;
        supplement: string;
        body_md: string | null;
      }
    | undefined;
  if (!row) return null;
  const parseArr = <T,>(s: string): T[] => {
    try {
      const v = JSON.parse(s);
      return Array.isArray(v) ? (v as T[]) : [];
    } catch {
      return [];
    }
  };
  return {
    paperId: row.id,
    flow: row.flow,
    theme: row.theme,
    tags: parseArr<FlowTag>(row.tags_json),
    mechanics: parseArr<MechanicSummary>(row.mechanics_json),
    supplement: row.supplement,
    bodyMd: row.body_md ?? "",
  };
}

/**
 * 既存ペーパーの本文 markdown を更新する (議論進行中の「ペーパーが更新されていく」用)。
 * ラウンドごとに base ブリーフ + 議論の経過 (まとめ/止揚) を焼き直して上書きする。
 */
export function updatePaperBody(paperId: string, bodyMd: string): void {
  getFlowDb()
    .prepare(`UPDATE discussion_paper SET body_md = ?, updated_at = ? WHERE id = ?`)
    .run(bodyMd, Date.now(), paperId);
}

/**
 * ペーパーの派生構造化フィールド (観点補足 / メカニクス) のみを更新する。
 * 議論後の本文リファインで body_md を書き換えた際、エクスポート等が読む構造化列を追従させる。
 * theme / tags / status は変えない (一覧タイトルや状態を保つ)。
 */
export function updatePaperDerived(
  paperId: string,
  derived: { supplement: string; mechanics: MechanicSummary[] }
): void {
  getFlowDb()
    .prepare(`UPDATE discussion_paper SET supplement = ?, mechanics_json = ?, updated_at = ? WHERE id = ?`)
    .run(derived.supplement, JSON.stringify(derived.mechanics), Date.now(), paperId);
}

/** セッションの最新ペーパー本文 markdown を返す (無ければ null)。 */
export function getPaperBodyBySession(sessionId: string): string | null {
  const row = getFlowDb()
    .prepare(
      `SELECT body_md FROM discussion_paper WHERE session_id = ? ORDER BY created_at DESC LIMIT 1`
    )
    .get(sessionId) as { body_md: string | null } | undefined;
  return row?.body_md ?? null;
}

/** 議論一覧の絞り込み状態。'all'=全件 / 'draft'=下書き / 'live'=進行中(未収束) / 'concluded'=結論あり。 */
export type FlowSessionState = "all" | "draft" | "live" | "concluded";

/** 議論一覧 1 行 (state は呼び出し側が concluded/status から導出)。 */
export interface FlowSessionRow {
  sessionId: string;
  theme: string;
  flow: string;
  status: string;
  createdAt: number;
  updatedAt: number;
  utterances: number;
  concluded: number | null;
}

export interface ListFlowSessionsResult {
  rows: FlowSessionRow[];
  /** 絞り込み後の総件数 (ページング用)。 */
  total: number;
}

/** state 絞り込みの WHERE 句 (correlated subquery で結論有無を見る)。'all' は常に真。 */
const FLOW_STATE_WHERE: Record<FlowSessionState, string> = {
  all: "1=1",
  draft: "dp.status = 'draft'",
  live: "dp.status != 'draft' AND COALESCE((SELECT fc.concluded FROM flow_conclusion fc WHERE fc.session_id = dp.session_id), 0) != 1",
  concluded: "COALESCE((SELECT fc.concluded FROM flow_conclusion fc WHERE fc.session_id = dp.session_id), 0) = 1",
};

/**
 * 議論一覧を state 絞り込み + ページングで返す (新しい順)。
 * total は同じ絞り込みでの全件数 (hasMore 判定用)。limit/offset は呼び出し側でクランプ済み前提。
 */
export function listFlowSessions(opts: {
  state?: FlowSessionState;
  limit: number;
  offset: number;
}): ListFlowSessionsResult {
  const db = getFlowDb();
  const where = FLOW_STATE_WHERE[opts.state ?? "all"];
  const total = (
    db.prepare(`SELECT COUNT(*) AS n FROM discussion_paper dp WHERE ${where}`).get() as { n: number }
  ).n;
  const rows = db
    .prepare(
      `SELECT dp.session_id AS sessionId, dp.theme AS theme, dp.flow AS flow, dp.status AS status,
              dp.created_at AS createdAt, dp.updated_at AS updatedAt,
              (SELECT COUNT(*) FROM flow_utterance fu WHERE fu.session_id = dp.session_id) AS utterances,
              (SELECT fc.concluded FROM flow_conclusion fc WHERE fc.session_id = dp.session_id) AS concluded
         FROM discussion_paper dp
        WHERE ${where}
        ORDER BY dp.created_at DESC
        LIMIT ? OFFSET ?`
    )
    .all(opts.limit, opts.offset) as FlowSessionRow[];
  return { rows, total };
}

/**
 * 1 議論 (session) の全永続データを削除する (下書き/不要議論の破棄)。
 * discussion_paper 本体 + session_id に紐づく派生行 (発話/結論/投票/スコア/コストログ/版履歴/
 * 結論キャッシュ) と、paper_id に紐づくラウンド行をまとめて 1 トランザクションで消す。
 * 対象が無ければ false (= 何も消さなかった)。
 */
export function deleteFlowSession(sessionId: string): boolean {
  const db = getFlowDb();
  const paperRows = db
    .prepare(`SELECT id FROM discussion_paper WHERE session_id = ?`)
    .all(sessionId) as Array<{ id: string }>;
  if (paperRows.length === 0) return false;
  const tx = db.transaction(() => {
    // paper_id 参照 (session_id を持たない append-only ラウンド) を先に消す。
    const delRound = db.prepare(`DELETE FROM discussion_paper_round WHERE paper_id = ?`);
    for (const { id } of paperRows) delRound.run(id);
    // session_id を持つ派生表をすべて削除。
    for (const table of [
      "flow_utterance",
      "flow_conclusion",
      "vote",
      "improvement_score",
      "llm_call_log",
      "discussion_paper_revision",
      "conclusion_cache",
    ]) {
      db.prepare(`DELETE FROM ${table} WHERE session_id = ?`).run(sessionId);
    }
    db.prepare(`DELETE FROM discussion_paper WHERE session_id = ?`).run(sessionId);
  });
  tx();
  return true;
}
