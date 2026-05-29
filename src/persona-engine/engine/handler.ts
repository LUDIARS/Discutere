/**
 * AI の応答 JSON を action に dispatch する。
 *
 * Phase 0 で受け付ける action:
 *   - skip (何もしない)
 *   - propose_hypothesis (DiscussionContextProvider.proposeHypothesis)
 *   - post_utterance (DiscussionContextProvider.postUtterance)
 *
 * 不正 JSON / 未知 action / 必須フィールド欠落 はすべて error として
 * RulesRepo.log に残し、 throw しない (engine 全体は走り続ける)。
 */

import type {
  DiscussionContextProvider,
  PostUtteranceInput,
  ProposeHypothesisInput,
} from "../context-provider.js";
import type { RulesRepo } from "../db/rules-repo.js";
import type { Logger } from "../types.js";

export interface HandleActionArgs {
  ruleId: string;
  personaId: string;
  workspaceId: string;
  sessionId: string | null;
  rawText: string;
  contextProvider: DiscussionContextProvider;
  rules: RulesRepo;
  logger: Logger;
}

export interface HandleActionResult {
  kind: "skip" | "propose_hypothesis" | "post_utterance" | "error";
  detail: string;
  createdId?: string;
}

export function handleAction(args: HandleActionArgs): HandleActionResult {
  const json = extractJson(args.rawText);
  if (!json) {
    const detail = `invalid JSON: ${args.rawText.slice(0, 80)}`;
    args.rules.log({
      rule_id: args.ruleId,
      action: "error",
      actor: "ai",
      detail,
    });
    args.logger.warn({ rule_id: args.ruleId }, detail);
    return { kind: "error", detail };
  }

  const action = typeof json.action === "string" ? json.action : "";

  if (action === "skip") {
    const reasoning =
      typeof json.reasoning === "string" ? json.reasoning : "(no reason)";
    args.rules.log({
      rule_id: args.ruleId,
      action: "skip",
      actor: "ai",
      detail: `persona ${args.personaId} skipped: ${reasoning}`,
    });
    return { kind: "skip", detail: reasoning };
  }

  if (action === "propose_hypothesis") {
    if (typeof json.statement !== "string" || json.statement.trim() === "") {
      return logErr(args, "propose_hypothesis without statement");
    }
    const input: ProposeHypothesisInput = {
      workspaceId: args.workspaceId,
      statement: json.statement,
      designGapId:
        typeof json.addresses_gap_id === "string"
          ? json.addresses_gap_id
          : typeof json.gap_id === "string"
            ? json.gap_id
            : null,
      byPersonaId: args.personaId,
      reasoning:
        typeof json.reasoning === "string" ? json.reasoning : undefined,
    };
    const { id } = args.contextProvider.proposeHypothesis(input);
    args.rules.log({
      rule_id: args.ruleId,
      action: "fire",
      actor: "ai",
      detail: `persona ${args.personaId} proposed hypothesis ${id}`,
    });
    args.logger.info(
      { rule_id: args.ruleId, persona_id: args.personaId, hypothesis_id: id },
      "hypothesis proposed"
    );
    return { kind: "propose_hypothesis", detail: input.statement, createdId: id };
  }

  if (action === "post_utterance") {
    if (typeof json.text !== "string" || json.text.trim() === "") {
      return logErr(args, "post_utterance without text");
    }
    if (!args.sessionId) {
      return logErr(args, "post_utterance requires sessionId (tick rule no session)");
    }
    const input: PostUtteranceInput = {
      workspaceId: args.workspaceId,
      sessionId: args.sessionId,
      text: json.text,
      byPersonaId: args.personaId,
      respondsTo:
        typeof json.responds_to === "string" ? json.responds_to : undefined,
    };
    const { id } = args.contextProvider.postUtterance(input);
    args.rules.log({
      rule_id: args.ruleId,
      action: "fire",
      actor: "ai",
      detail: `persona ${args.personaId} posted utterance ${id}`,
    });
    args.logger.info(
      { rule_id: args.ruleId, persona_id: args.personaId, utterance_id: id },
      "utterance posted"
    );
    return { kind: "post_utterance", detail: input.text, createdId: id };
  }

  return logErr(args, `unknown action: ${action}`);
}

function logErr(args: HandleActionArgs, detail: string): HandleActionResult {
  args.rules.log({
    rule_id: args.ruleId,
    action: "error",
    actor: "ai",
    detail,
  });
  args.logger.warn({ rule_id: args.ruleId }, detail);
  return { kind: "error", detail };
}

/**
 * LLM 応答から最初の JSON object を抽出。 周辺の説明文を許容。
 */
function extractJson(text: string): Record<string, unknown> | null {
  const trimmed = text.trim();
  // try direct parse
  try {
    const v = JSON.parse(trimmed);
    if (v && typeof v === "object" && !Array.isArray(v))
      return v as Record<string, unknown>;
  } catch {
    /* fallthrough */
  }
  // try to find first { ... } balanced
  const start = trimmed.indexOf("{");
  if (start < 0) return null;
  let depth = 0;
  for (let i = start; i < trimmed.length; i += 1) {
    const c = trimmed[i];
    if (c === "{") depth += 1;
    else if (c === "}") {
      depth -= 1;
      if (depth === 0) {
        const candidate = trimmed.slice(start, i + 1);
        try {
          const v = JSON.parse(candidate);
          if (v && typeof v === "object" && !Array.isArray(v))
            return v as Record<string, unknown>;
        } catch {
          /* fall through */
        }
      }
    }
  }
  return null;
}

/** test 用 export */
export const __test = { extractJson };
