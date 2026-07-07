/**
 * 回答型ペルソナ生成: 質問票構築 + 回答ベクトル解析 + flow_persona 保存。
 */

import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import type { LLMInvokeArgs, LLMResult } from "../../src/persona-engine/llm/client.js";

const TMP_DIR = path.resolve(".tmp/flow-persona-questionnaire-test");
fs.rmSync(TMP_DIR, { recursive: true, force: true });
fs.mkdirSync(TMP_DIR, { recursive: true });
process.env.DATABASE_PATH = path.join(TMP_DIR, "persona-questionnaire.db");

const { _resetFlowDb } = await import("../../src/flow/db/connection.js");
_resetFlowDb();

const {
  buildPersonaQuestionnaire,
  analyzeQuestionnaireAnswers,
  createPersonaFromQuestionnaireAnswers,
} = await import("../../src/flow/persona-questionnaire.js");
const { listPoolPersonas } = await import("../../src/flow/persona-pool.js");
const { DIM } = await import("../../src/flow/sentiment-vector.js");

class StaticLlm {
  constructor(private readonly text: string) {}
  async invoke(_args: LLMInvokeArgs): Promise<LLMResult> {
    return { ok: true, text: this.text };
  }
}

{
  const q = await buildPersonaQuestionnaire({
    gameTitle: "モンスターストライク",
    mechanicsContext: "引っぱり操作、友情コンボ、ガチャ、協力プレイ、チュートリアル",
    userVoices: ["チュートリアルが分かりやすいと続けやすい", "ガチャの納得感が大事"],
    questionCount: 6,
  });

  assert.equal(q.source, "fallback", "LLMなしではfallback質問票");
  assert.equal(q.baselineVector.length, DIM as number, "baseline vector dim");
  assert.ok(q.questions.some((item) => item.kind === "preference_metric"), "嗜好指標質問あり");
  assert.ok(q.questions.some((item) => item.kind === "game_usage"), "利用/仕様質問あり");
  assert.ok(q.questions.some((item) => item.kind === "game_specific"), "タイトル特化質問あり");
  console.log("  [ok] persona questionnaire fallback coverage + baseline vector");
}

{
  const llm = new StaticLlm(
    JSON.stringify([
      {
        id: "taste-reward",
        kind: "preference_metric",
        metric: "報酬嗜好",
        question: "報酬テンポをどの程度重視しますか?",
        options: ["かなり重視", "普通", "気にしない"],
        vectorHints: ["報酬", "テンポ", "満足"],
        weight: 1.4,
      },
      {
        id: "usage-tutorial",
        kind: "game_usage",
        metric: "チュートリアル理解",
        question: "初回説明はどの粒度がよいですか?",
        options: ["細かい", "最低限", "後から確認"],
        vectorHints: ["理解負荷", "初回体験"],
        weight: 1.2,
      },
      {
        id: "monst-friend-combo",
        kind: "game_specific",
        metric: "友情コンボ理解",
        question: "友情コンボの説明で何を知りたいですか?",
        vectorHints: ["モンスト", "友情コンボ"],
        weight: 1,
      },
    ])
  );
  const q = await buildPersonaQuestionnaire({
    gameTitle: "モンスターストライク",
    questionCount: 3,
    llm,
  });

  assert.equal(q.source, "llm", "LLM質問票を採用");
  assert.equal(q.questions.length, 3, "指定件数");
  assert.equal(q.questions[0].id, "taste-reward", "LLM id を保持");
  console.log("  [ok] persona questionnaire LLM JSON normalized");
}

{
  const questionnaire = await buildPersonaQuestionnaire({
    gameTitle: "モンスターストライク",
    mechanicsContext: "チュートリアルでは引っぱり操作と友情コンボを説明する",
    questionCount: 5,
  });
  const answers = Object.fromEntries(
    questionnaire.questions.map((q, i) => [
      q.id,
      i % 2 === 0
        ? "チュートリアルは短く、実際に触りながら理解したい。報酬と成長の納得感は重視する。"
        : "協力プレイや友情コンボの使いどころを具体例で知りたい。",
    ])
  );
  const analysis = analyzeQuestionnaireAnswers(questionnaire, answers);
  assert.equal(analysis.affectVector.length, DIM as number, "affect vector dim");
  assert.equal(analysis.responseVector.length, DIM as number, "response vector dim");
  assert.equal(analysis.answerVectors.length, questionnaire.questions.length, "全回答を解析");

  const result = await createPersonaFromQuestionnaireAnswers({
    questionnaire,
    answers,
    name: "回答 太郎",
    persist: true,
  });

  assert.equal(result.saved, true, "保存される");
  assert.equal(result.persona.name, "回答 太郎", "指定名を保持");
  assert.equal(result.persona.learningSource, "questionnaire", "learningSource=questionnaire");
  assert.equal(result.persona.affectVector.length, DIM as number, "persona vector dim");
  assert.ok(listPoolPersonas({ origin: "generated" }).some((p) => p.id === result.persona.id), "flow_persona に保存");
  console.log("  [ok] questionnaire answers create persisted persona + vector deltas");
}

console.log("persona-questionnaire tests: all passed");
