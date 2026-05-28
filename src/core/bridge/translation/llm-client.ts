import { readFileSync } from "node:fs";
import { resolve } from "node:path";

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

export class AnthropicLlmClient implements LlmClient {
  async extract(rawContent: string, affectCandidates: string[], mechanicCandidates: string[]): Promise<TranslationResult> {
    const apiKey = process.env.ANTHROPIC_API_KEY;
    if (!apiKey) return new MockLlmClient().extract(rawContent, affectCandidates, mechanicCandidates);

    const prompt = [
      readFileSync(resolve(process.cwd(), "src/core/bridge/translation/prompts/axis-1-learning.md"), "utf8"),
      `text=${rawContent}`,
      `affects=${affectCandidates.join(",")}`,
      `mechanics=${mechanicCandidates.join(",")}`,
    ].join("\n");

    const res = await fetch("https://api.anthropic.com/v1/messages", {
      method: "POST",
      headers: {
        "content-type": "application/json",
        "x-api-key": apiKey,
        "anthropic-version": "2023-06-01",
      },
      body: JSON.stringify({
        model: process.env.ANTHROPIC_MODEL ?? "claude-3-5-haiku-latest",
        max_tokens: 256,
        messages: [{ role: "user", content: prompt }],
      }),
    });
    if (!res.ok) return new MockLlmClient().extract(rawContent, affectCandidates, mechanicCandidates);

    const body: any = await res.json();
    const text = body?.content?.[0]?.text;
    if (typeof text !== "string") return new MockLlmClient().extract(rawContent, affectCandidates, mechanicCandidates);
    try {
      return JSON.parse(text) as TranslationResult;
    } catch {
      return new MockLlmClient().extract(rawContent, affectCandidates, mechanicCandidates);
    }
  }
}
