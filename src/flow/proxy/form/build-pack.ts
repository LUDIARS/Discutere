/**
 * 公開レビュー → 人物パック (契約 v1) の決定的な形成 (統合レビュー §3 S5、U1: Histrio ができるまでの仮置き)。
 *
 * LLM は使わない (U2: Histrio の「LLM 不使用」を形成にも適用)。1 話者 × 1 ジャンルごとに:
 *   1. 評価用に一部のゲームを伏せる (held-out。形成・根拠・束に入れない)
 *   2. 件数・好意的/否定的・ゲーム数・話題にした観点から材料の充足度を決める
 *   3. 20D (sentiment-core) の好意的レビュー − 否定的レビュー を補助値として計算する (正負各 2 件未満は null)
 *   4. 望むもの / 不満なもの = 好意的 / 否定的なレビューでよく話題にした観点
 *   5. 根拠の発言を好意的・否定的を交互に最大 12 件、立場カードを根拠の文そのものから最大 6 件
 * 同じ入力からは同じパックが出る (時刻だけは呼び出し側が渡す)。
 */

import { createHash } from "node:crypto";
import { ASP_KEYS, buildVector, extractTextFeatures, subtract, type TextFeatures } from "@ludiars/sentiment-core";
import {
  MAX_EVIDENCE,
  MAX_EVIDENCE_CHARS,
  MAX_LIKED_GAMES,
  PROXY_CONTRACT_VERSION,
  type Confidence,
  type PackEvidence,
  type PersonaPack,
  type StanceCard,
} from "../contract.js";
import { VECTOR_SPEC_VERSION } from "../../sentiment-vector.js";

export const FORM_RULE_VERSION = "proxy-form-v1";
const MAX_STANCE_CARDS = 6;
const PACK_TTL_MS = 90 * 24 * 60 * 60 * 1000;

/** 形成の入力 1 件 (1 話者の 1 レビュー)。生の外部 ID は speakerKey に変換済みで渡す。 */
export interface FormReview {
  gameSlug: string;
  gameTitle: string;
  text: string;
  recommended: boolean;
  sourceKind: string;
}

export interface FormThresholds {
  /** medium 以上に必要な件数・ゲーム数・観点数 (統合レビュー §1 採用基準)。 */
  minUtterances: number;
  minGames: number;
  minAspects: number;
  /** high に必要な件数・ゲーム数。 */
  highUtterances: number;
  highGames: number;
  /** gap (数値差) を作るのに必要な好意的・否定的それぞれの件数。 */
  minPolarity: number;
}

export const DEFAULT_THRESHOLDS: FormThresholds = {
  minUtterances: 10,
  minGames: 3,
  minAspects: 3,
  highUtterances: 20,
  highGames: 5,
  minPolarity: 2,
};

function sha(text: string): string {
  return createHash("sha256").update(text).digest("hex");
}

/** 伏せるゲーム: 4 本以上なら 1/4 (最低 1 本)。並びは speakerKey 付きの hash で決める (決定的)。 */
export function pickHeldOutGames(speakerKey: string, games: readonly string[]): Set<string> {
  if (games.length < 4) return new Set();
  const count = Math.max(1, Math.floor(games.length / 4));
  const ordered = [...games].sort((a, b) => sha(`${speakerKey}:${a}`).localeCompare(sha(`${speakerKey}:${b}`)));
  return new Set(ordered.slice(0, count));
}

/** 本文中のリンク・@ハンドルを伏せる (出典や本人へ辿る手がかりを根拠に残さない。U3)。 */
export function sanitizeReviewText(text: string): string {
  return text
    .replace(/https?:\/\/\S+/gi, "(リンク省略)")
    .replace(/(^|\s)@[A-Za-z0-9_]{2,}/g, "$1(ID省略)");
}

/** 文の区切りで max 文字に収める (文の途中で切らない。1 文目が長すぎるときだけ字数で切る)。 */
export function truncateAtSentence(text: string, max: number): string {
  const clean = sanitizeReviewText(text).replace(/\s+/g, " ").trim();
  if (clean.length <= max) return clean;
  const sentences = clean.match(/[^。！？!?.]+[。！？!?.]?/g) ?? [clean];
  let out = "";
  for (const s of sentences) {
    if ((out + s).length > max) break;
    out += s;
  }
  return out.trim() || clean.slice(0, max);
}

function mentionedAspects(f: TextFeatures): string[] {
  return ASP_KEYS.filter((a) => (f.aspectMentions[a] ?? 0) > 0);
}

/** 好意的 (or 否定的) なレビューで 2 回以上話題にした観点を多い順に (同数は次元名順)。 */
function topAspects(features: readonly TextFeatures[], max: number): string[] {
  const counts = new Map<string, number>();
  for (const f of features) for (const a of mentionedAspects(f)) counts.set(a, (counts.get(a) ?? 0) + 1);
  return [...counts.entries()]
    .filter(([, n]) => n >= 2)
    .sort((a, b) => b[1] - a[1] || a[0].localeCompare(b[0]))
    .slice(0, max)
    .map(([a]) => `asp.${a}`);
}

function confidenceOf(
  n: number,
  games: number,
  aspects: number,
  t: FormThresholds
): { confidence: Confidence; reasons: string[] } {
  const reasons: string[] = [];
  if (n < t.minUtterances) reasons.push(`発言が ${t.minUtterances} 件未満 (${n} 件)`);
  if (games < t.minGames) reasons.push(`ゲームが ${t.minGames} 本未満 (${games} 本)`);
  if (aspects < t.minAspects) reasons.push(`話題にした観点が ${t.minAspects} 未満 (${aspects})`);
  if (reasons.length > 0) return { confidence: "low", reasons };
  return { confidence: n >= t.highUtterances && games >= t.highGames ? "high" : "medium", reasons };
}

interface Scored {
  review: FormReview;
  features: TextFeatures;
  aspects: string[];
}

/** 根拠: 観点を多く話題にした順に、好意的・否定的を交互に選ぶ。 */
function selectEvidence(items: readonly Scored[]): Scored[] {
  const rank = (a: Scored, b: Scored) =>
    b.aspects.length - a.aspects.length || b.review.text.length - a.review.text.length || a.review.text.localeCompare(b.review.text);
  const pos = items.filter((i) => i.review.recommended).sort(rank);
  const neg = items.filter((i) => !i.review.recommended).sort(rank);
  const out: Scored[] = [];
  for (let k = 0; out.length < MAX_EVIDENCE && (k < pos.length || k < neg.length); k++) {
    if (pos[k]) out.push(pos[k]);
    if (neg[k] && out.length < MAX_EVIDENCE) out.push(neg[k]);
  }
  return out;
}

/** 立場カード: 根拠の発言のうち観点を最も多く含む文を、そのまま主張にする (要約しない = 捏造しない)。 */
function ruleStanceCards(evidence: readonly PackEvidence[]): StanceCard[] {
  const cards: StanceCard[] = [];
  const seen = new Set<string>();
  for (const e of evidence) {
    if (cards.length >= MAX_STANCE_CARDS) break;
    const sentences = (e.text.match(/[^。！？!?.]+[。！？!?.]?/g) ?? [e.text]).map((s) => s.trim()).filter((s) => s.length >= 10);
    const best = sentences
      .map((s) => ({ s, n: mentionedAspects(extractTextFeatures(s)).length }))
      .filter((x) => x.n > 0)
      .sort((a, b) => b.n - a.n || a.s.localeCompare(b.s))[0];
    if (!best || seen.has(best.s)) continue;
    seen.add(best.s);
    cards.push({
      id: `S${cards.length + 1}`,
      claim: best.s.slice(0, 200),
      polarity: e.polarity,
      evidenceIds: [e.id],
      extractor: "rule",
    });
  }
  return cards;
}

const FIRST_PERSON = ["私", "僕", "俺", "自分", "わたし", "ぼく", "おれ"];

/** 口調メモ (頻度集計のみ)。 */
function styleNotes(texts: readonly string[]): string[] {
  const notes: string[] = [];
  const counts = FIRST_PERSON.map((w) => ({ w, n: texts.filter((t) => t.includes(w)).length })).sort((a, b) => b.n - a.n);
  if (counts[0] && counts[0].n >= 2) notes.push(`一人称は「${counts[0].w}」`);
  const avg = texts.reduce((s, t) => s + t.length, 0) / Math.max(1, texts.length);
  notes.push(avg >= 200 ? "長めに書く" : avg >= 80 ? "ほどほどの長さで書く" : "短く書く");
  const exclaim = texts.filter((t) => /[!！]/.test(t)).length / Math.max(1, texts.length);
  if (exclaim >= 0.5) notes.push("感嘆符が多い");
  return notes;
}

export interface BuildPackArgs {
  speakerKey: string;
  genre: string;
  genreSpecVersion: number;
  reviews: readonly FormReview[];
  now: number;
  thresholds?: FormThresholds;
}

/** 評価専用の伏せた発言 (判別テスト M3 に使う。パック・束・議論には載せない)。 */
export interface HeldOutText {
  speakerKey: string;
  genre: string;
  game: string;
  text: string;
}

export interface BuiltPack {
  pack: PersonaPack;
  heldOutTexts: HeldOutText[];
}

/** 1 話者 × 1 ジャンルのパックを作る。根拠が 1 件も無ければ null (契約上パックにできない)。 */
export function buildPersonaPack(args: BuildPackArgs): BuiltPack | null {
  const t = args.thresholds ?? DEFAULT_THRESHOLDS;
  // 同じ本文の重複 (転載・二重取得) は 1 件にする。
  const unique = [...new Map(args.reviews.map((r) => [sha(r.text.trim()), r])).values()];
  const games = [...new Set(unique.map((r) => r.gameSlug))].sort();
  const heldOutGames = pickHeldOutGames(args.speakerKey, games);
  const formation = unique.filter((r) => !heldOutGames.has(r.gameSlug));
  if (formation.length === 0) return null;

  const scored: Scored[] = formation.map((review) => {
    const features = extractTextFeatures(review.text, { votedUp: review.recommended });
    return { review, features, aspects: mentionedAspects(features) };
  });
  const posFeatures = scored.filter((s) => s.review.recommended).map((s) => s.features);
  const negFeatures = scored.filter((s) => !s.review.recommended).map((s) => s.features);
  const formationGames = new Set(formation.map((r) => r.gameSlug)).size;
  const aspectCount = new Set(scored.flatMap((s) => s.aspects)).size;
  const { confidence, reasons } = confidenceOf(formation.length, formationGames, aspectCount, t);

  const gapReady = posFeatures.length >= t.minPolarity && negFeatures.length >= t.minPolarity;
  const evidence: PackEvidence[] = selectEvidence(scored).map((s, i) => ({
    id: `E${i + 1}`,
    revision: sha(s.review.text.trim()).slice(0, 16),
    text: truncateAtSentence(s.review.text, MAX_EVIDENCE_CHARS),
    sourceKind: s.review.sourceKind,
    gameTitle: s.review.gameTitle,
    polarity: s.review.recommended ? "pos" : "neg",
    // 取得層の postedAt は取得時刻の代用なので投稿日として載せない (統合レビュー Astra 指摘)。
    postedAt: null,
  }));

  const heldOut = [...heldOutGames].map((game) => {
    const rs = unique.filter((r) => r.gameSlug === game);
    const yes = rs.filter((r) => r.recommended).length;
    return { game: rs[0]?.gameTitle ?? game, recommended: yes * 2 >= rs.length };
  });

  const inputFingerprint = sha(
    JSON.stringify(unique.map((r) => [r.gameSlug, r.recommended, sha(r.text)]).sort())
  ).slice(0, 12);

  const heldOutTexts: HeldOutText[] = unique
    .filter((r) => heldOutGames.has(r.gameSlug))
    .map((r) => ({ speakerKey: args.speakerKey, genre: args.genre, game: r.gameTitle, text: truncateAtSentence(r.text, MAX_EVIDENCE_CHARS) }));

  const pack: PersonaPack = {
    contractVersion: PROXY_CONTRACT_VERSION,
    personaKind: "observedSpeaker",
    speakerKey: args.speakerKey,
    genre: args.genre,
    genreSpecVersion: args.genreSpecVersion,
    profileVersion: `${FORM_RULE_VERSION}:${inputFingerprint}`,
    formation: { method: "rule", ruleVersion: FORM_RULE_VERSION },
    window: { observedMonths: null, generatedAt: args.now, expiresAt: args.now + PACK_TTL_MS },
    sufficiency: {
      utterances: formation.length,
      positive: posFeatures.length,
      negative: negFeatures.length,
      games: formationGames,
      aspects: aspectCount,
      confidence,
      reasons,
    },
    affect: { vector: buildVector(scored.map((s) => s.features)), vectorSpecVersion: VECTOR_SPEC_VERSION },
    designGap: gapReady
      ? {
          vector: subtract(buildVector(posFeatures), buildVector(negFeatures)),
          method: "pos-minus-neg",
          wants: topAspects(posFeatures, 3),
          complaints: topAspects(negFeatures, 3),
          reason: null,
        }
      : {
          vector: null,
          method: "pos-minus-neg",
          wants: topAspects(posFeatures, 3),
          complaints: topAspects(negFeatures, 3),
          reason: `好意的・否定的なレビューがそれぞれ ${t.minPolarity} 件以上必要 (好意的 ${posFeatures.length} / 否定的 ${negFeatures.length})`,
        },
    conditionalGaps: [],
    stanceCards: ruleStanceCards(evidence),
    evidence,
    styleNotes: styleNotes(formation.map((r) => r.text)),
    likedGames: likedGames(formation),
    heldOut,
    revoked: false,
  };
  return { pack, heldOutTexts };
}

/** 推薦したゲーム名。契約の上限を超える人は、長く書いたゲームから MAX_LIKED_GAMES 本だけ残す。 */
function likedGames(formation: readonly { recommended: boolean; gameTitle: string; text: string }[]): string[] {
  const longest = new Map<string, number>();
  for (const r of formation) {
    if (!r.recommended) continue;
    longest.set(r.gameTitle, Math.max(longest.get(r.gameTitle) ?? 0, r.text.length));
  }
  return [...longest]
    .sort((a, b) => b[1] - a[1] || a[0].localeCompare(b[0]))
    .slice(0, MAX_LIKED_GAMES)
    .map(([title]) => title)
    .sort();
}
