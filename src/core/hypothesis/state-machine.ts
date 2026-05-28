export type HypothesisState =
  | "draft"
  | "proposed"
  | "under_theory_validation"
  | "validated_by_theory"
  | "under_emotion_validation"
  | "validated_by_emotion"
  | "integrated"
  | "rejected";

export type HypothesisAction = "submit" | "validate_theory" | "validate_emotion" | "session_end" | "integrate" | "reject";

const terminal = new Set<HypothesisState>(["integrated", "rejected"]);

export function canTransition(from: HypothesisState, action: HypothesisAction): { ok: true; to: HypothesisState } | { ok: false; error: string } {
  if (terminal.has(from) && action !== "reject") {
    return { ok: false, error: `cannot transition from terminal state: ${from}` };
  }

  if (action === "submit") return from === "draft" ? { ok: true, to: "proposed" } : { ok: false, error: "submit requires draft" };
  if (action === "validate_theory") return from === "proposed" ? { ok: true, to: "under_theory_validation" } : { ok: false, error: "validate theory requires proposed" };
  if (action === "validate_emotion") return from === "validated_by_theory" ? { ok: true, to: "under_emotion_validation" } : { ok: false, error: "validate emotion requires validated_by_theory" };
  if (action === "session_end") {
    if (from === "under_theory_validation") return { ok: true, to: "validated_by_theory" };
    if (from === "under_emotion_validation") return { ok: true, to: "validated_by_emotion" };
    return { ok: false, error: "session end transition is invalid for current state" };
  }
  if (action === "integrate") return from === "validated_by_emotion" ? { ok: true, to: "integrated" } : { ok: false, error: "integrate requires validated_by_emotion" };
  if (action === "reject") return { ok: true, to: "rejected" };

  return { ok: false, error: "unknown transition" };
}
