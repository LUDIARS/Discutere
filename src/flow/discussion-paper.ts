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
}

/** ペルソナへの配布内容 (ターンごとに組み立て) */
export interface PersonaPaper {
  theme: string;
  supplement: string;
  mechanicsText: string;
  previousRoundsText: string;
  currentRoundUtterances: string[];
  userOpinionsText?: string;
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

/** ユーザ意見テキストを構築する (RAG 用)。 */
function buildUserOpinionsText(voices: ContextVoice[]): string {
  if (voices.length === 0) return "";
  const lines = voices
    .slice(0, 5)
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
  };
}

/** PersonaPaper を LLM に渡すプロンプト文字列に変換する。 */
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

/** discussion_paper を DB に保存し paperId を返す。 */
export function persistPaper(paper: Omit<DiscussionPaper, "paperId" | "rounds">): string {
  const db = getFlowDb();
  const paperId = randomUUID();
  const now = Date.now();
  db.prepare(
    `INSERT INTO discussion_paper (id, flow, session_id, theme, tags_json, mechanics_json, supplement, created_at, updated_at)
     VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)`
  ).run(
    paperId,
    "discussion",
    paper.sessionId,
    paper.theme,
    JSON.stringify(paper.tags),
    JSON.stringify(paper.mechanics),
    paper.supplement,
    now,
    now
  );
  return paperId;
}
