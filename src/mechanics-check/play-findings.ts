/** Compose conditional numerical findings and explicitly missing model conditions. */
import type { ComparedStatement, DesignGap, MechanicsCheckRequest, PlayEnvelope, PlayEnvelopeDifference, PlayScenario } from "./contracts.js";

function modelStatement(text: string, quote: string): ComparedStatement {
  return { text, evidence: [{ referenceId: "playModel", quote }] };
}
function missingConditions(scenario: PlayScenario, envelope: PlayEnvelope): string[] {
  const missing: string[] = [];
  if (!scenario.skillBand) missing.push("技能帯が未指定。技能帯間の差をランダム分散と解釈できない。");
  if (!scenario.assumptions.information) missing.push("プレイヤーの情報条件が未指定。");
  if (!scenario.assumptions.initialState) missing.push("開始状態が未指定。");
  if (scenario.assumptions.timeLimitSeconds === undefined && scenario.assumptions.trialCount === undefined) missing.push("制限時間または試行回数が未指定。");
  if (!scenario.predictedBaseline) missing.push("本人の予測基準値が未指定。上振れ・下振れは unknown。");
  if (!scenario.exhaustive) missing.push("結果が非網羅。列挙した部分の到達幅であり、全体の限界は unknown。");
  if (scenario.outcomes.some((o) => o.probability === undefined)) missing.push("同時確率が未指定または部分指定。期待値と分散は unknown。");
  if (envelope.determinant === "unknown") missing.push("事象の決定要因が unknown。");
  return missing;
}
function scenarioConditions(scenario: PlayScenario): string[] {
  const a = scenario.assumptions;
  return [
    "入力模型に対する条件付き計算。実プレイや面白さの証明ではない。",
    ...(scenario.skillBand ? [`技能帯: ${scenario.skillBand}`] : []),
    ...(a.information ? [`情報: ${a.information}`] : []),
    ...(a.initialState ? [`開始状態: ${a.initialState}`] : []),
    ...(a.timeLimitSeconds === undefined ? [] : [`制限時間: ${a.timeLimitSeconds}秒`]),
    ...(a.trialCount === undefined ? [] : [`試行回数: ${a.trialCount}`]),
    ...(a.other ?? []),
  ];
}
export function composePlayFindings(request: MechanicsCheckRequest, envelopes: PlayEnvelope[]): { differences: PlayEnvelopeDifference[]; gaps: DesignGap[] } {
  const model = request.playModel;
  if (!model) return { differences: [], gaps: [] };
  const gaps: DesignGap[] = [];
  const differences = envelopes.map((envelope, i): PlayEnvelopeDifference => {
    const scenario = model.scenarios[i];
    const conditions = scenarioConditions(scenario);
    const missing = missingConditions(scenario, envelope);
    const unknowns = [...missing, "入力模型の条件付き計算であり、実プレイでこの到達幅が生じるかは未確認。"];
    const left = scenario.predictedBaseline
      ? modelStatement(`本人の予測基準値: ${scenario.predictedBaseline.value} ${model.metric.unit}`, JSON.stringify(scenario.predictedBaseline))
      : { text: "本人の予測基準値は未提示。", evidence: [] };
    const right = modelStatement(`列挙した同時結果の幅: ${envelope.min}..${envelope.max} ${model.metric.unit} (${envelope.rangeScope})`, `"id":${JSON.stringify(scenario.id)}`);
    const finding: PlayEnvelopeDifference = { kind: "play_envelope", target: scenario.id, left, right, status: "unknown", conditions, unknowns, envelope };
    // Calculation describes the supplied model; it does not judge a prediction as proved.
    if (missing.length) gaps.push({ kind: "missing_condition", target: scenario.id, left, right, status: "unknown", conditions, unknowns: missing });
    return finding;
  });
  return { differences, gaps };
}
