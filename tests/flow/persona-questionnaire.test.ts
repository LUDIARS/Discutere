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
  PREFERENCE_AXES,
  genericPersonaQuestionTemplates,
  genericPersonaQuestionBank,
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
  const templates = genericPersonaQuestionTemplates();
  const questions = genericPersonaQuestionBank("モンスターストライク");
  const ids = new Set(questions.map((q) => q.id));
  assert.equal(templates.length, questions.length, "テンプレートから質問集を構築");
  assert.ok(questions.length >= 18, "汎用質問集は十分な件数を持つ");
  assert.equal(ids.size, questions.length, "汎用質問IDは重複しない");
  assert.deepEqual(
    [...new Set(questions.slice(0, 3).map((q) => q.kind))].sort(),
    ["game_specific", "game_usage", "preference_metric"],
    "先頭3問で主要分類を網羅"
  );
  assert.ok(templates.every((q) => q.essence.trim().length > 0), "質問の本質を持つ");
  assert.equal(questions[0].scoring?.["達成感"]?.["style.achiever"], 0.9, "選択肢スコアを質問に展開");
  assert.ok(questions.some((q) => q.metric === "学習スタイル"), "チュートリアル/学習の汎用質問あり");
  assert.ok(questions.some((q) => q.question.includes("モンスターストライク")), "対象タイトルを質問文に反映");
  console.log("  [ok] generic persona question bank coverage");
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
      q.options?.[0] ??
        (i % 2 === 0
        ? "チュートリアルは短く、実際に触りながら理解したい。報酬と成長の納得感は重視する。"
        : "協力プレイや友情コンボの使いどころを具体例で知りたい。"),
    ])
  );
  const analysis = analyzeQuestionnaireAnswers(questionnaire, answers);
  assert.equal(analysis.affectVector.length, DIM as number, "affect vector dim");
  assert.equal(analysis.responseVector.length, DIM as number, "response vector dim");
  assert.equal(analysis.preferenceVector.length, PREFERENCE_AXES.length, "preference vector dim");
  assert.ok(analysis.topPreferenceAxes.length > 0, "嗜好軸スコアあり");
  assert.ok((analysis.preferenceScores["style.achiever"] ?? 0) > 0, "選択肢からAchieverが加点される");
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
