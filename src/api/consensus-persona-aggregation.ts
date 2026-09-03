export interface PersonaConsensusUtterance {
  id: string;
  content: string;
  postedAt: number;
  agree: boolean | null;
  score: number | null;
  reasoning: string | null;
}

export interface PersonaConsensusSource extends PersonaConsensusUtterance {
  opinionScore: number;
  personaId: string;
  displayName: string;
  traits: readonly string[];
}

export interface PersonaConsensusSummary {
  personaId: string;
  displayName: string;
  traits: readonly string[];
  utterances: PersonaConsensusUtterance[];
  opinionScore: number;
  consensusScore: number;
  agreeRatio: number;
}

/** @implements SPEC-THALEIA-PERSONA-CONSENSUS Persona 別の意見・合意スコアを集計する副作用なしの変換。 */
export function aggregatePersonaConsensus(
  rows: readonly PersonaConsensusSource[]
): PersonaConsensusSummary[] {
  const grouped = new Map<string, PersonaConsensusSummary>();
  for (const row of rows) {
    const current = grouped.get(row.personaId) ?? {
      personaId: row.personaId,
      displayName: row.displayName,
      traits: row.traits,
      utterances: [],
      opinionScore: 0,
      consensusScore: 0,
      agreeRatio: 0,
    };
    current.utterances.push({
      id: row.id,
      content: row.content,
      postedAt: row.postedAt,
      agree: row.agree,
      score: row.score,
      reasoning: row.reasoning,
    });
    current.opinionScore += row.opinionScore;
    current.consensusScore += row.score ?? 0;
    grouped.set(row.personaId, current);
  }

  return [...grouped.values()]
    .map((persona) => ({
      ...persona,
      utterances: [...persona.utterances].sort((a, b) => a.postedAt - b.postedAt),
      agreeRatio: ratioOfAgreed(persona.utterances),
    }))
    .sort((a, b) => a.displayName.localeCompare(b.displayName));
}

/** @implements SPEC-THALEIA-PERSONA-CONSENSUS 未採点の発話は分母に含めない。 */
export function ratioOfAgreed(utterances: readonly Pick<PersonaConsensusUtterance, "agree">[]): number {
  const scored = utterances.filter((utterance) => utterance.agree !== null);
  if (scored.length === 0) return 0;
  return scored.filter((utterance) => utterance.agree).length / scored.length;
}
