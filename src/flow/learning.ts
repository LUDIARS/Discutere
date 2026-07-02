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
import {
  collectAndImport,
  deriveSlug,
  type AutoCrawlSource,
  type AutoCrawlSpec,
} from "./learning-autocrawl.js";
import { upsertGameMechanics, type GameMechanicEntry } from "./games-md.js";

type Core = ReturnType<typeof createCore>;

/** 学習フローが受ける 1 意見 (収集元は問わない。匿名)。 */
export interface LearningOpinion {
  /** 意見本文 (個人名・アカウント名を含めない) */
  content: string;
  /** 投稿時刻 epoch ms (省略時は記録時刻) */
  postedAt?: number;
}

/** 自動収集モードの指定 (① 類似ゲームの自動収集)。 */
export interface LearningCrawlSpec {
  /** 収集ソース群 (既定は config.flow.autoCrawl.sources)。空なら収集しない。 */
  sources: AutoCrawlSource[];
  /** 検索クエリ (省略時はテーマ)。 */
  query?: string;
  /** 1 ソースあたり取込上限 (既定 200)。 */
  maxItems?: number;
  /** youtube ソース用 API キー (無ければ youtube はスキップ)。 */
  youtubeApiKey?: string;
  /** ソース別の追加パラメータ (steam appId / website urls)。自動経路では通常未指定。 */
  specBySource?: Partial<Record<AutoCrawlSource, Pick<AutoCrawlSpec, "appId" | "urls">>>;
}

export interface LearningFlowDeps {
  /** Discatier Core (メカニクス記録 + KG 外部発話書込)。 */
  core: Core;
  /** 収集する意見 (ゲーム感想チャンネル等から取得済みの匿名テキスト)。 */
  opinions?: LearningOpinion[];
  /** 記録するメカニクス。 */
  mechanics?: GameMechanicEntry[];
  /**
   * 自動収集モード (① 類似ゲームの自動収集)。指定すると、起動だけで (opinions 未供給でも)
   * テーマで **複数ソース横断クロール → KG 取込** する。学習は明示起動なので充足ゲートは
   * 挟まない (常に収集する。ゲート付きの事前学習は議論/改善前の information-gate が担う)。
   * 未指定なら従来どおり opinions / mechanics の記録のみ。
   */
  crawl?: LearningCrawlSpec;
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
  /** 自動収集の取得→取込関数 (テスト用に差し替え)。既定は learning-autocrawl.collectAndImport。 */
  collectAndImportFn?: typeof collectAndImport;
  log?: (msg: string) => void;
  warn?: (msg: string) => void;
}

export interface LearningFlowResult {
  gameSlug: string;
  opinionsRecorded: number;
  mechanicsRecorded: number;
  /** 付与された極性の内訳 (positive/negative/neutral 件数)。 */
  polarityBreakdown: Record<string, number>;
  /** 自動収集モードでクロール取込した件数 (crawl 未指定なら 0)。 */
  crawledImported: number;
  /** ソース別のクロール取込内訳 (crawl 未指定なら空)。 */
  crawledBySource: Record<string, number>;
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
    // 出所ラベル (08): クロール/学習取込経路のメカニクスは source="crawl" を付けて永続する
    // (investigate が読み戻すとき curated と区別できる。明示済みの source は尊重)。
    const labeled = mechanics.map((m) => (m.source ? m : { ...m, source: "crawl" as const }));
    // data/games/<slug>.md (議論フローの調査が読む正路)
    mechanicsRecorded = upsertGameMechanics({ gamesDir, slug, title: gameTitle, mechanics: labeled });
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

  // ── 自動収集モード (① 類似ゲームの自動収集) ─────────────────────────────────
  // opinions 未供給でも、テーマで複数ソース横断クロール → KG 取込する。
  let crawledImported = 0;
  const crawledBySource: Record<string, number> = {};
  if (deps.crawl && deps.crawl.sources.length > 0) {
    const doCollect = deps.collectAndImportFn ?? collectAndImport;
    const { sources, query, maxItems, youtubeApiKey, specBySource } = deps.crawl;
    const theme = query?.trim() || gameTitle;
    for (const source of sources) {
      const spec: AutoCrawlSpec = { source, query: theme, ...(specBySource?.[source] ?? {}) };
      try {
        const r = await doCollect({
          core,
          theme,
          slug,
          workspaceId,
          spec,
          maxItems,
          youtubeApiKey,
          log,
          warn,
        });
        crawledImported += r.imported;
        crawledBySource[source] = (crawledBySource[source] ?? 0) + r.imported;
      } catch (err) {
        // 1 ソースの失敗は学習全体を止めない (graceful degrade)。
        warn(`自動収集失敗 (source=${source}): ${(err as Error).message}`);
      }
    }
    log(`自動収集 ${crawledImported} 件を KG に取込 (${sources.join("/")})`);
  }

  return {
    gameSlug: slug,
    opinionsRecorded,
    mechanicsRecorded,
    polarityBreakdown,
    crawledImported,
    crawledBySource,
  };
}
