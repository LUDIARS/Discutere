/**
 * ペルソナ 1 ターン実行 (discussion.md step 5, respec 10 の runFlow 分割)。
 *
 * ペーパー組み立て → LLM invoke → 発話レコード化。
 * stance はセッション固定 (respec 01, personaStance で導出)。
 * 永続・通知は呼び出し側 director が行う (onUtterance / persistUtterance の順序維持)。
 */

import { randomUUID } from "node:crypto";
import type { LLMClient } from "../persona-engine/llm/client.js";
import { getConfig } from "../config.js";
import { personaStance, type FlowPersona } from "./personas.js";
import type { FlowUtteranceRecord } from "./director.js";
import type { VoiceCache } from "./voice-cache.js";
import {
  buildPersonaPaper,
  buildPaperSystem,
  buildPersonaUserPrompt,
  type ContextVoice,
  type DiscussionPaper,
} from "./discussion-paper.js";
import type { investigateTheme } from "./investigate.js";
import { withCostLog } from "./cost-logger.js";

export interface PersonaTurnArgs {
  persona: FlowPersona;
  round: number;
  turn: number;
  paper: DiscussionPaper;
  sessionId: string;
  paperId: string;
  /** フロー種別 (コストログ用)。 */
  flow: string;
  llm: LLMClient;
  /** 当ラウンドのこれまでの発話 (プロンプトの可変部)。 */
  roundUtterances: ReadonlyArray<{ personaName: string; text: string }>;
  /** 発話時 RAG のセッションキャッシュ (item5: 全ペルソナ共有)。 */
  voiceCache: VoiceCache;
  /** investigate の生結果 (YouTube 補完の声。確定ペーパー経路は null)。 */
  investigation: Awaited<ReturnType<typeof investigateTheme>> | null;
  /** 機密タグ用の想定意見 (synthetic)。 */
  syntheticOpinions: string[];
  log: (msg: string) => void;
  warn: (msg: string) => void;
}

/**
 * 1 ターンを実行し発話レコードを返す。空応答は null (発話に数えない)。
 * LLM エラーは isError=true のレコードとして返す (握り潰さない)。
 */
export async function runPersonaTurn(args: PersonaTurnArgs): Promise<FlowUtteranceRecord | null> {
  const {
    persona,
    round,
    turn,
    paper,
    sessionId,
    paperId,
    flow,
    llm,
    roundUtterances,
    voiceCache,
    investigation,
    syntheticOpinions,
    log,
    warn,
  } = args;
  const cfg = getConfig();
  const stance = personaStance(persona);

  // ユーザ意見 RAG (item5: セッションキャッシュ経由で全ペルソナ共有)
  const userVoices: ContextVoice[] = voiceCache.lookup([paper.theme], cfg.flow.paperRichness.voices);

  // YouTube コメントを userVoices に追加 (補完時のみ。確定ペーパー経路は investigation=null)
  if (investigation && investigation.youtubeUsed && investigation.youtubeComments.length > 0) {
    const ytVoices: ContextVoice[] = investigation.youtubeComments
      .slice(0, 3)
      .map((c) => ({ content: c, source: "youtube" }));
    userVoices.push(...ytVoices);
  }

  // ペーパー組み立て
  const personaPaper = buildPersonaPaper({
    paper,
    persona,
    stance,
    currentRoundUtterances: [...roundUtterances],
    userVoices,
    syntheticOpinions,
  });
  // 安定部 (議題/メカニクス) は system に置き SDK の cache_control で session 内再利用 (E)。
  // 可変部 (前ラウンド/当ラウンド/ユーザの声) + persona 固有は user メッセージへ。
  const personaSystem = buildPaperSystem(personaPaper);
  const prompt = buildPersonaUserPrompt(personaPaper, stance, persona);

  // LLM 呼び出し (コストログ付き)
  const logged = withCostLog(llm, {
    flow,
    sessionId,
    round,
    turn,
    role: persona.role,
    persona: persona.name,
    location: persona.role === "facilitator" ? "facilitator" : "utterance",
  });

  let utteranceText: string;
  let isError = false;

  const result = await logged.invoke({
    system: personaSystem,
    prompt,
    model: persona.model,
  });

  if (!result.ok) {
    utteranceText = `[エラー: ${result.error}]`;
    isError = true;
    warn(`ターン ${round}-${turn} (${persona.name}) LLM エラー: ${result.error}`);
  } else {
    utteranceText = result.text.trim();
    // 空応答は発話に数えない
    if (!utteranceText) {
      log(`ターン ${round}-${turn} (${persona.name}): 空応答 (スキップ)`);
      return null;
    }
  }

  return {
    id: randomUUID(),
    sessionId,
    paperId,
    round,
    turn,
    personaId: persona.id,
    personaName: persona.name,
    role: persona.role,
    stance,
    text: utteranceText,
    isError,
    possessionName: persona.possession?.label,
  };
}
