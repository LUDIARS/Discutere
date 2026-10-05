/**
 * 振り分けたペーパー調整指示 (paper-review-intent) に、種類ごとの材料を足して
 * applyPaperEdit へ渡す指示文を組み立てる。
 *
 *   - adjust        : 返信本文をそのまま指示にする。
 *   - voices        : 指定語 (無ければ議題) でクロール → 取込し、集めた外部の声を根拠として渡す。
 *   - similar_games : 挙げたゲームのメカニクスを data/games から引き、代替 (参考) として渡す。
 *                     既存データに無いゲームは LLM の一般知識で補うよう指示する。
 *
 * クロール / 検索 / メカニクス取得は注入境界 (テストで差し替え可)。失敗は warn して続行する。
 */

import type { LLMClient } from "../persona-engine/llm/client.js";
import type { ContextVoice } from "./discussion-paper.js";
import type { FlowTag } from "./tags.js";
import { investigateTheme } from "./investigate.js";
import { gatherEvidence } from "./paper-review.js";
import type { PaperReviewIntent } from "./paper-review-intent.js";

/** 類似ゲームとして引くゲーム数の上限。 */
const MAX_SIMILAR_GAMES = 5;
/** 1 ゲームあたり指示に載せるメカニクス数の上限。 */
const MAX_MECHANICS_PER_GAME = 8;

export interface PaperIntentDeps {
  /** 議論の議題 (検索語が無い時の既定)。 */
  theme: string;
  tags: readonly FlowTag[];
  gamesDir?: string;
  /** 収集済みの外部の声検索 (RAG)。無ければ外部の声は集められない。 */
  listExternalVoices?: (terms: string[], limit: number) => ContextVoice[];
  /** 根拠段落の要約に使う LLM (省略時は箇条書き)。 */
  llm?: LLMClient;
  /** 指定語でクロール → KG 取込する。未指定ならクロールせず収集済みの声だけを使う。 */
  crawl?: (query: string) => Promise<{ imported: number }>;
  /** メカニクス取得 (テスト用差し替え)。既定は investigateTheme。 */
  investigate?: typeof investigateTheme;
  warn?: (msg: string) => void;
}

export interface PaperIntentInstruction {
  /** applyPaperEdit へ渡す指示。null なら反映しない (材料が見つからなかった等)。 */
  instruction: string | null;
  /** スレッドへ出す処理結果の一言。 */
  notice: string;
}

async function buildVoicesInstruction(
  intent: PaperReviewIntent,
  deps: PaperIntentDeps
): Promise<PaperIntentInstruction> {
  const query = intent.terms.join(" ") || deps.theme;
  let imported = 0;
  if (deps.crawl) {
    try {
      imported = (await deps.crawl(query)).imported;
    } catch (e) {
      deps.warn?.(`外部の声のクロール失敗 (${query}): ${(e as Error).message}`);
    }
  }
  if (!deps.listExternalVoices) {
    return { instruction: null, notice: "外部の声の検索が使えないため、取り込めませんでした。" };
  }
  const evidence = await gatherEvidence({
    topic: query,
    listExternalVoices: deps.listExternalVoices,
    llm: deps.llm,
    warn: deps.warn,
  });
  const crawled = imported > 0 ? `新たに ${imported} 件をクロールして取り込み、` : "";
  if (evidence.voices.length === 0) {
    return { instruction: null, notice: `${crawled}「${query}」に関する外部の声は見つかりませんでした。` };
  }
  return {
    instruction:
      `${intent.text}\n\n` +
      "次の外部の声 (出所付き) を根拠として、ペーパーの該当箇所 (観点補足 / ゲーム内容 / 論点) に取り込んでください。" +
      "出所は残し、個人名は書かないこと。\n\n" +
      evidence.suggestion,
    notice: `${crawled}「${query}」の外部の声 ${evidence.voices.length} 件を根拠として反映します。`,
  };
}

async function buildSimilarGamesInstruction(
  intent: PaperReviewIntent,
  deps: PaperIntentDeps
): Promise<PaperIntentInstruction> {
  const investigate = deps.investigate ?? investigateTheme;
  const games = intent.terms.slice(0, MAX_SIMILAR_GAMES);
  const found: string[] = [];
  const missing: string[] = [];
  for (const game of games) {
    try {
      const r = await investigate({ theme: game, tags: deps.tags, gamesDir: deps.gamesDir, warn: deps.warn });
      if (r.mechanics.length === 0) {
        missing.push(game);
        continue;
      }
      for (const m of r.mechanics.slice(0, MAX_MECHANICS_PER_GAME)) {
        found.push(`- [${game}] ${m.name}${m.description ? `: ${m.description}` : ""}`);
      }
    } catch (e) {
      deps.warn?.(`類似ゲームのメカニクス取得失敗 (${game}): ${(e as Error).message}`);
      missing.push(game);
    }
  }

  const lines = [
    intent.text,
    "",
    "類似するゲームのメカニクスを、このゲームのメカニクスの代替 (参考) として「メカニクス」に追加してください。" +
      "追加する項目の説明には「(参考: ゲーム名)」を付け、このゲーム本来のメカニクスと区別すること。",
  ];
  if (found.length > 0) lines.push("", "既存データにある類似ゲームのメカニクス:", ...found);
  if (missing.length > 0) {
    lines.push("", `既存データに無いゲーム (${missing.join("、")}) は、代表的なメカニクスを一般知識から補ってください。`);
  }
  if (games.length === 0) {
    lines.push("", "ゲーム名の指定が無いため、議題に近い類似ゲームを 1〜3 本選び、その代表的なメカニクスを補ってください。");
  }

  const notice =
    games.length === 0
      ? "類似ゲームの指定が無いため、議題に近いゲームを選んでメカニクスの代替を補います。"
      : `類似ゲーム (${games.join("、")}) のメカニクスを代替として反映します` +
        (found.length > 0 ? ` (既存データ ${found.length} 件)` : "") +
        "。";
  return { instruction: lines.join("\n"), notice };
}

/** 振り分けた指示に材料を足し、applyPaperEdit 用の指示文とスレッド通知を返す。 */
export async function buildIntentInstruction(
  intent: PaperReviewIntent,
  deps: PaperIntentDeps
): Promise<PaperIntentInstruction> {
  switch (intent.kind) {
    case "voices":
      return buildVoicesInstruction(intent, deps);
    case "similar_games":
      return buildSimilarGamesInstruction(intent, deps);
    default:
      return { instruction: intent.text, notice: "" };
  }
}
