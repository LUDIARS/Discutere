/** Pure arithmetic over supplied joint outcomes; never synthesizes combinations. */
import { MechanicsCheckError, type Determinant, type PlayEnvelope, type PlayModel, type PlayScenario } from "./contracts.js";

function determinant(model: PlayModel, scenario: PlayScenario): Determinant {
  const included = new Set(scenario.outcomes.flatMap((outcome) => outcome.eventIds));
  const factors = new Set(model.events.filter((event) => included.has(event.id)).map((event) => event.determinant));
  if (factors.has("unknown")) return "unknown";
  if (factors.has("mixed") || (factors.has("random") && factors.has("player_skill"))) return "mixed";
  if (factors.has("random")) return "random";
  if (factors.has("player_skill")) return "player_skill";
  return "deterministic";
}
function checked(value: number): number {
  if (!Number.isFinite(value)) throw new MechanicsCheckError("invalid_input", "playModel: numeric calculation overflow");
  return value;
}

/** The model must have passed validateMechanicsCheckRequest at the caller boundary. */
export function calculatePlayEnvelopes(model: PlayModel): PlayEnvelope[] {
  return model.scenarios.map((scenario) => {
    const baseline = scenario.predictedBaseline?.value ?? null;
    const complete = scenario.exhaustive && scenario.outcomes.every((o) => o.probability !== undefined);
    // A fully specified zero-probability combination is not reachable support.
    const support = complete ? scenario.outcomes.filter((o) => (o.probability ?? 0) > 0) : scenario.outcomes;
    const min = Math.min(...support.map((o) => o.value));
    const max = Math.max(...support.map((o) => o.value));
    // Probability validation happened at the boundary. No inferred independent marginals.
    const expectedValue = complete ? checked(scenario.outcomes.reduce((sum, o) => checked(sum + checked(o.value * (o.probability ?? 0))), 0)) : null;
    const variance = expectedValue === null ? null : checked(scenario.outcomes.reduce((sum, o) => {
      if (o.probability === 0) return sum;
      const delta = checked(o.value - expectedValue);
      return checked(sum + checked(checked(delta * delta) * (o.probability ?? 0)));
    }, 0));
    const higher = model.metric.direction === "higher";
    return {
      scenarioId: scenario.id,
      metric: { ...model.metric },
      skillBand: scenario.skillBand ?? null,
      determinant: determinant(model, scenario),
      rangeScope: scenario.exhaustive ? "enumerated_complete" : "enumerated_partial",
      min, max, predictedBaseline: baseline,
      upside: baseline === null ? null : Math.max(0, checked(higher ? max - baseline : baseline - min)),
      downside: baseline === null ? null : Math.max(0, checked(higher ? baseline - min : max - baseline)),
      expectedValue, variance,
      probabilityStatus: !scenario.exhaustive ? "non_exhaustive" : complete ? "complete" : "missing_or_partial",
    };
  });
}
