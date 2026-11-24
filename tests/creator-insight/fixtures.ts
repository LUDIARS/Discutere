/** creator-insight のテスト用に、設問・回答例・特徴カード例を読む。すべて架空データ。 */
import path from "node:path";
import { fileURLToPath } from "node:url";
import { loadQuestionnaire, readJsonFile, type Questionnaire } from "../../scripts/creator-insight/lib/questionnaire.js";
import { parseAnswers, type LifeScriptAnswers } from "../../scripts/creator-insight/score-life-script.js";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const SKILL = path.resolve(HERE, "../../skills/creator-insight");

export const questionnairePath = path.join(SKILL, "data/life-script-questionnaire.ja.json");
export const followupsPath = path.join(SKILL, "data/life-script-followups.ja.json");

/** @implements DI-CI-08 */
export function questionnaire(): Questionnaire {
  return loadQuestionnaire(questionnairePath);
}

/** @implements DI-CI-09 */
export function exampleAnswers(q: Questionnaire): LifeScriptAnswers {
  return parseAnswers(readJsonFile(path.join(SKILL, "examples/life-script-answers.example.json")), q);
}

/** @implements DI-CI-09 */
export function expectedCard(): { lifeScript: unknown } {
  const card = readJsonFile(path.join(SKILL, "examples/feature-card.example.json")) as { questionnaire: { lifeScript: unknown } };
  return card.questionnaire;
}

/** 全 likert 項目に同じ評定を入れた回答 (逆転前の値)。 @implements DI-CI-09 */
export function uniformAnswers(q: Questionnaire, value: number): LifeScriptAnswers {
  const answers: Record<string, number> = {};
  for (const s of q.sections) if (s.type === "likert") for (const i of s.items) answers[i.id] = value;
  return { questionnaire: q.id, revision: q.revision, answers, forcedChoice: {} };
}
