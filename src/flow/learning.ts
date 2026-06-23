/**
 * 学習フロー (learning.md)。
 *
 * ゲームタイトル + テーマに即したユーザ意見 / メカニクスを収集・記録する。
 * **LLM による議論はしない** (ペルソナ発話・ラウンド・投票なし)。議論フローの前段として
 * 実行し、議論フロー step 2 (調査) / step 3 (ペーパー) の素材を仕込む。
 *
 *   - 意見    → KG 外部発話 (importExternalUtterances, source="feedback", 匿名)。
 *               極性は感情カスケード (cascade.ts) で付与する。議論フローの
 *               listExternalVoices (KG 読み) が同じ KG から読める。
 *   - メカニクス → data/games/<slug>.md frontmatter (investigate.loadMechanics が読む)
 *                  + Discatier Core mechanics (記録)。
 *
 * 個人データは保存しない (authorId は匿名固定、authorName は持たない。CLAUDE.md 準拠)。
 */

import type { createCore } from "../core/index.js";
import type { CascadeClients } from "../crawler/sentiment/cascade.js";
import { cascadeSentiment } from "../crawler/sentiment/cascade.js";
import { importExternalUtterances } from "../crawler/sources/importer.js";
import type { ExternalUtterance } from "../crawler/sources/types.js";
import { deriveSlug } from "./learning-autocrawl.js";
import { upsertGameMechanics, type GameMechanicEntry } from "./games-md.js";

type Core = ReturnType<typeof createCore>;

/** 学習フローが受ける 1 意見 (収集元は問わない。匿名)。 */
export interface LearningOpinion {
  /** 意見本文 (個人名・アカウント名を含めない) */
  content: string;
  /** 投稿時刻 epoch ms (省略時は記録時刻) */
  postedAt?: number;
}

export interface LearningFlowDeps {
  /** Discatier Core (メカニクス記録 + KG 外部発話書込)。 */
  core: Core;
  /** 収集する意見 (ゲーム感想チャンネル等から取得済みの匿名テキスト)。 */
  opinions?: LearningOpinion[];
  /** 記録するメカニクス。 */
  mechanics?: GameMechanicEntry[];
  /** 感情カスケードの LLM (省略時は Tier 0 辞書のみ。LLM 議論はしない)。 */
  sentimentClients?: CascadeClients;
  /** ワークスペース ID (既定 knowledge)。 */
  workspaceId?: string;
  /**
   * slug 明示指定。省略時は deriveSlug(gameTitle)。deriveSlug は ASCII 化を試み、空なら
   * 元タイトル (空白→ハイフン) にフォールバックするため日本語タイトルでも非空になる
   * (autocrawl / 情報ゲートと同じ slug 規約。材料が同じ slug に揃う)。
   */
  slug?: string;
  /** data/games ディレクトリ (既定 ./data/games)。 */
  gamesDir?: string;
  log?: (msg: string) => void;
  warn?: (msg: string) => void;
}

export interface LearningFlowResult {
  gameSlug: string;
  opinionsRecorded: number;
  mechanicsRecorded: number;
  /** 付与された極性の内訳 (positive/negative/neutral 件数)。 */
  polarityBreakdown: Record<string, number>;
}

/**
 * 学習フローを実行する。議論は一切しない (収集・記録のみ)。
 */
export async function runLearningFlow(
  gameTitle: string,
  theme: string,
  deps: LearningFlowDeps
): Promise<LearningFlowResult> {
  const {
    core,
    opinions = [],
    mechanics = [],
    sentimentClients = {},
    workspaceId = "knowledge",
    gamesDir = "./data/games",
    log = (m) => console.log(`[learning] ${m}`),
    warn = (m) => console.warn(`[learning/warn] ${m}`),
  } = deps;

  // deriveSlug は ASCII 化に失敗しても元タイトル等にフォールバックするため非空 (日本語 OK)。
  const slug = deps.slug ?? deriveSlug(gameTitle);
  if (!slug) {
    throw new Error(`slug を導出できません (title="${gameTitle}")。deps.slug を明示してください。`);
  }
  log(`学習フロー開始: "${gameTitle}" (slug=${slug}, テーマ="${theme}")`);

  // ── メカニクス記録 ───────────────────────────────────────────────────────
  let mechanicsRecorded = 0;
  if (mechanics.length > 0) {
    // data/games/<slug>.md (議論フローの調査が読む正路)
    mechanicsRecorded = upsertGameMechanics({ gamesDir, slug, title: gameTitle, mechanics });
    // Discatier Core mechanics にも記録 (name/description)
    for (const m of mechanics) {
      if (!m.name) continue;
      try {
        core.repos.mechanic.create({
          workspaceId,
          gameId: slug,
          name: m.name,
          description: m.description,
        });
      } catch (err) {
        warn(`メカニクス Core 記録に失敗 (${m.name}): ${(err as Error).message}`);
      }
    }
    log(`メカニクス ${mechanicsRecorded} 件を data/games/${slug}.md + Core に記録`);
  }

  // ── 意見収集 + 極性付与 + KG 記録 ────────────────────────────────────────
  const polarityBreakdown: Record<string, number> = { positive: 0, negative: 0, neutral: 0 };
  const items: ExternalUtterance[] = [];

  for (let i = 0; i < opinions.length; i++) {
    const op = opinions[i];
    const text = op.content?.trim();
    if (!text) continue;

    let sentiment;
    try {
      sentiment = await cascadeSentiment(text, undefined, sentimentClients);
    } catch (err) {
      warn(`感情カスケード失敗 (意見 ${i}): ${(err as Error).message}`);
      sentiment = { polarity: "neutral" as const, score: 0, confidence: 0, tier: 0 as const };
    }
    polarityBreakdown[sentiment.polarity] = (polarityBreakdown[sentiment.polarity] ?? 0) + 1;

    items.push({
      source: "feedback",
      // 匿名 + 冪等: スレッド内連番。個人 ID は持たない。
      nativeId: `learning:${slug}:${op.postedAt ?? "t"}:${i}`,
      gameSlug: slug,
      threadKey: `learning:${slug}`,
      content: text,
      postedAt: op.postedAt ?? Date.now(),
      authorId: "anon", // 個人アンカーを持たない (CLAUDE.md 個人データ非保管)
      sourceUrl: `discutere://game-feedback/${slug}`,
      // 極性を賛否シグナルに反映 (positive/negative のみ。中立はシグナルなし)
      signal:
        sentiment.polarity === "positive"
          ? { votedUp: true }
          : sentiment.polarity === "negative"
            ? { votedUp: false }
            : undefined,
    });
  }

  let opinionsRecorded = 0;
  if (items.length > 0) {
    const res = importExternalUtterances(core, items, { workspaceId });
    opinionsRecorded = res.utterances;
    log(`意見 ${opinionsRecorded} 件を KG に記録 (skip ${res.skipped})`);
  }

  return { gameSlug: slug, opinionsRecorded, mechanicsRecorded, polarityBreakdown };
}
