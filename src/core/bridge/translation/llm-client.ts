export interface TranslationResult {
  affect?: { mood: string; confidence: number };
  mechanic?: { name: string; confidence: number };
}

export interface LlmClient {
  extract(rawContent: string, affectCandidates: string[], mechanicCandidates: string[]): Promise<TranslationResult>;
}

export class MockLlmClient implements LlmClient {
  async extract(rawContent: string, affectCandidates: string[], mechanicCandidates: string[]): Promise<TranslationResult> {
    const text = rawContent.toLowerCase();
    const affect = affectCandidates.find((a) => text.includes(a.toLowerCase()));
    const mechanic = mechanicCandidates.find((m) => text.includes(m.toLowerCase()));
    return {
      affect: affect ? { mood: affect, confidence: 0.8 } : (text.includes("fun") ? { mood: "fun", confidence: 0.7 } : undefined),
      mechanic: mechanic ? { name: mechanic, confidence: 0.75 } : undefined,
    };
  }
}
