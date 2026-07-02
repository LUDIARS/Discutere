/**
 * フローセッションのセットアップ (discussion.md step 1〜2, respec 10 の runFlow 分割)。
 *
 *   [1] 調査 (investigate + メカニクス LLM 増補) — 確定ペーパー (paperOverride) があれば省略
 *   [2] ディスカッションペーパー初期化 (body_md 正本化 + persistPaper)
 *
 * 挙動は director.ts にインラインだった時と同一 (リファクタのみ)。deps は引数で受ける
 * (既存の DI スタイル踏襲。テストで Mock 注入可能)。
 */

import type { LLMClient } from "../persona-engine/llm/client.js";
import type { FlowTag } from "./tags.js";
import { getConfig } from "../config.js";
import type { VoiceCache } from "./voice-cache.js";
import {
  persistPaper,
  type DiscussionPaper,
} from "./discussion-paper.js";
import { investigateTheme, type YoutubeSearchFn, type MechanicSummary } from "./investigate.js";

/** 人間が調整・承認した確定ペーパーの上書き値 (investigate の出力を置き換える)。 */
export interface PaperOverride {
  mechanics: MechanicSummary[];
  supplement: string;
  /**
   * 確定した議論ブリーフ本文の正本 markdown (Web の Notion 風編集ゲートで調整したもの)。
   * 指定時はこれを各 LLM の system に直接載せる。未指定なら mechanics/supplement から生成する。
   */
  bodyMd?: string;
}

export interface FlowSetupArgs {
  theme: string;
  tags: readonly FlowTag[];
  /** フロー種別 (ペーパーに記録)。 */
  flow: string;
  sessionId: string;
  llm: LLMClient;
  /** 発話時 RAG のセッションキャッシュ (メカニクス増補の材料 lookup に使う)。 */
  voiceCache: VoiceCache;
  youtubeSearch?: YoutubeSearchFn;
  gamesDir?: string;
  paperOverride?: PaperOverride;
  log: (msg: string) => void;
  warn: (msg: string) => void;
}

export interface FlowSetupResult {
  paper: DiscussionPaper;
  paperId: string;
  /** investigate の生結果 (確定ペーパー経路では null。YouTube 補完の声に使う)。 */
  investigation: Awaited<ReturnType<typeof investigateTheme>> | null;
}

/**
 * 調査 + ペーパー初期化を行い、永続済みの DiscussionPaper を返す。
 */
export async function setupFlowPaper(args: FlowSetupArgs): Promise<FlowSetupResult> {
  const { theme, tags, flow, sessionId, llm, voiceCache, youtubeSearch, gamesDir, log, warn } = args;
  const cfg = getConfig();

  // ── [1] 調査 ──────────────────────────────────────────────────────────────
  // 確定ペーパー (人間レビュー済) があれば investigate を省略し、その内容で議論する。
  const override = args.paperOverride;
  let investigation: Awaited<ReturnType<typeof investigateTheme>> | null = null;
  let mechanics: MechanicSummary[];
  let supplement: string;
  // 議論ブリーフ本文の正本 markdown (ハイブリッド源泉)。確定ペーパーの bodyMd を優先し、
  // 無ければ構造化フィールドから生成する (全経路で body_md を持たせ各 LLM が直接参照)。
  let bodyMd: string | undefined;
  if (override) {
    mechanics = override.mechanics;
    supplement = override.supplement;
    bodyMd = override.bodyMd;
    log(`確定ペーパー使用: investigate スキップ (メカニクス ${mechanics.length} 件)`);
  } else {
    log(`調査開始: "${theme}" (タグ: [${tags.join(", ")}])`);
    investigation = await investigateTheme({
      theme,
      tags,
      gamesDir,
      youtubeSearch,
      youtubeMaxComments: cfg.flow.youtubeMaxComments,
      warn,
    });
    mechanics = investigation.mechanics;
    supplement = (await import("./tags.js")).paperSupplement(tags);
    // メカニクスを LLM で目標件数まで増補 (感想を根拠に。paperOverride 経路は増補済みなので対象外)。
    // 材料 (感想) が無ければ増補しない (抽出根拠が無く LLM コストの無駄)。
    const enrichVoices = voiceCache.lookup([theme], cfg.flow.paperRichness.voices);
    if (cfg.flow.paperRichness.enrichMechanics && enrichVoices.length > 0) {
      mechanics = await (await import("./mechanic-extract.js")).enrichMechanics({
        theme,
        existing: mechanics,
        voices: enrichVoices,
        llm,
        target: cfg.flow.paperRichness.mechanicsTarget,
        model: cfg.flow.paperRichness.enrichModel || undefined,
        warn,
      });
      log(`メカニクス増補後: ${mechanics.length} 件`);
    }
  }

  // ── [2] ディスカッションペーパー初期化 ─────────────────────────────────
  // bodyMd (正本 md) が未指定なら構造化フィールドから生成する (Discord / 非レビュー経路)。
  if (!bodyMd || !bodyMd.trim()) {
    bodyMd = (await import("./paper-markdown.js")).paperDraftToMarkdown({
      theme,
      tags: [...tags],
      supplement,
      mechanics,
    });
  }
  const paperId = persistPaper(
    { sessionId, theme, tags: [...tags], mechanics, supplement, bodyMd },
    flow
  );
  const paper: DiscussionPaper = {
    paperId,
    sessionId,
    theme,
    tags: [...tags],
    mechanics,
    supplement,
    bodyMd,
    rounds: [],
  };

  return { paper, paperId, investigation };
}
