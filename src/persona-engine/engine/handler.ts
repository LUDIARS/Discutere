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
import { SEED_RULE_IDS } from "../seeds/rules.js";
import type { Logger, RuleSeed, RuleTriggerType } from "../types.js";

export interface HandleActionArgs {
  ruleId: string;
  personaId: string;
  workspaceId: string;
  sessionId: string | null;
  triggerEvent?: { kind: string; payload?: unknown };
  rawText: string;
  contextProvider: DiscussionContextProvider;
  rules: RulesRepo;
  logger: Logger;
}

export interface HandleActionResult {
  kind:
    | "skip"
    | "propose_hypothesis"
    | "post_utterance"
    | "add_rule"
    | "remove_rule"
    | "error";
  detail: string;
  createdId?: string;
}

/**
 * AI が add_rule で持ち込もうとする rule の禁則 (= 議論を破壊する rule の事前 reject)。
 * Concordia の禁則を議論モード向けに拡張。
 */
const FORBIDDEN_RULE_PATTERNS: Array<{ pattern: RegExp; reason: string }> = [
  {
    pattern: /無音|沈黙|silence|quiet[-_ ]?check/i,
    reason: "無音 / 沈黙確認系 rule は禁止 (rule_log で見えるため不要)",
  },
  {
    pattern: /進捗(どう|確認|聞|を聞|ping)|progress[-_ ]?(check|ping)/i,
    reason: "進捗確認系 rule は禁止 (status endpoint で取得可)",
  },
  {
    pattern: /汎用雑談|general[-_ ]?chitchat|small[-_ ]?talk[-_ ]?random/i,
    reason: "ロール無関係な汎用雑談 rule は禁止 (persona の役割に紐づけること)",
  },
  {
    pattern: /全\s*persona|all[-_ ]?personas?[-_ ]?(fire|act|invoke)/i,
    reason: "全 persona 一括動員 rule は禁止 (LLM コスト暴騰防止)",
  },
];

function checkForbiddenRule(rule: {
  id: string;
  description?: string | null;
  instructions: string;
}): { ok: true } | { ok: false; reason: string } {
  const blob = `${rule.id}\n${rule.description ?? ""}\n${rule.instructions}`;
  for (const { pattern, reason } of FORBIDDEN_RULE_PATTERNS) {
    if (pattern.test(blob)) return { ok: false, reason };
  }
  return { ok: true };
}

export const _FORBIDDEN_RULE_PATTERNS = FORBIDDEN_RULE_PATTERNS;
export const _checkForbiddenRule = checkForbiddenRule;

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
            : inferDesignGapIdFromTrigger(args.triggerEvent),
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

  if (action === "add_rule") {
    return handleAddRule(args, json);
  }
  if (action === "remove_rule") {
    return handleRemoveRule(args, json);
  }

  if (action === "post_utterance") {
    if (typeof json.text !== "string" || json.text.trim() === "") {
      return logErr(args, "post_utterance without text");
    }
    // tick rule は session context を持たない。 json.hypothesis_id から
    // 紐付く議論 session を解決する。 解決できなければ error ではなく skip。
    let sessionId = args.sessionId;
    if (!sessionId) {
      const hypothesisId =
        typeof json.hypothesis_id === "string" ? json.hypothesis_id : undefined;
      sessionId =
        (hypothesisId && args.contextProvider.findDiscussionSession?.({
          workspaceId: args.workspaceId,
          hypothesisId,
        })) ||
        null;
    }
    if (!sessionId) {
      return {
        kind: "skip",
        detail: "post_utterance: 議論 session を解決できないため skip (tick rule)",
      };
    }
    const input: PostUtteranceInput = {
      workspaceId: args.workspaceId,
      sessionId,
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

function handleAddRule(
  args: HandleActionArgs,
  json: Record<string, unknown>
): HandleActionResult {
  const ruleSpec = json.rule;
  if (!ruleSpec || typeof ruleSpec !== "object") {
    return logErr(args, "add_rule without rule object");
  }
  const r = ruleSpec as Record<string, unknown>;
  const id = typeof r.id === "string" ? r.id.trim() : "";
  const trigger = typeof r.trigger_type === "string" ? r.trigger_type : "";
  const instructions = typeof r.instructions === "string" ? r.instructions : "";
  if (!/^[a-z][a-z0-9-]{1,63}$/.test(id)) {
    return logErr(args, `add_rule id invalid (must be kebab 2-64): ${id}`);
  }
  if (trigger !== "tick" && trigger !== "event") {
    return logErr(args, `add_rule trigger_type must be tick|event: ${trigger}`);
  }
  if (!instructions || instructions.length < 16 || instructions.length > 4000) {
    return logErr(args, "add_rule instructions length must be 16-4000");
  }

  const forbidden = checkForbiddenRule({
    id,
    description: typeof r.description === "string" ? r.description : null,
    instructions,
  });
  if (!forbidden.ok) {
    return logErr(args, `add_rule forbidden: ${forbidden.reason}`);
  }

  // 既存 (= seed 含む) の id は再 insert しない (= ignore)
  if (args.rules.get(id)) {
    return logErr(args, `add_rule id already exists: ${id}`);
  }

  const seed: RuleSeed = {
    id,
    description: typeof r.description === "string" ? r.description : undefined,
    trigger_type: trigger as RuleTriggerType,
    tick_sec:
      typeof r.tick_sec === "number" && r.tick_sec >= 5 && r.tick_sec <= 3600
        ? r.tick_sec
        : null,
    event_kind: typeof r.event_kind === "string" ? r.event_kind : null,
    target: typeof r.target === "string" ? r.target : null,
    cooldown_sec:
      typeof r.cooldown_sec === "number" && r.cooldown_sec >= 0 && r.cooldown_sec <= 86400
        ? r.cooldown_sec
        : 60,
    instructions,
  };

  args.rules.insertOrIgnore(seed, { addedBy: `ai:${args.personaId}` });
  args.rules.log({
    rule_id: id,
    action: "add",
    actor: "ai",
    detail: `persona ${args.personaId} added rule ${id} via ${args.ruleId}`,
  });
  args.logger.info(
    { rule_id: id, persona_id: args.personaId, via: args.ruleId },
    "rule added by AI"
  );
  return { kind: "add_rule", detail: id, createdId: id };
}

function handleRemoveRule(
  args: HandleActionArgs,
  json: Record<string, unknown>
): HandleActionResult {
  const ruleId = typeof json.rule_id === "string" ? json.rule_id : "";
  const reasoning = typeof json.reasoning === "string" ? json.reasoning : "(no reason)";
  if (!ruleId) {
    return logErr(args, "remove_rule without rule_id");
  }
  if (SEED_RULE_IDS.has(ruleId)) {
    return logErr(args, `remove_rule rejected: ${ruleId} is a protected seed`);
  }
  const existing = args.rules.get(ruleId);
  if (!existing) {
    return logErr(args, `remove_rule unknown id: ${ruleId}`);
  }
  args.rules.remove(ruleId, `ai:${args.personaId}`, reasoning);
  args.rules.log({
    rule_id: ruleId,
    action: "remove",
    actor: "ai",
    detail: `persona ${args.personaId} removed rule ${ruleId}: ${reasoning}`,
  });
  args.logger.info(
    { rule_id: ruleId, persona_id: args.personaId, reasoning },
    "rule removed by AI"
  );
  return { kind: "remove_rule", detail: ruleId };
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

function inferDesignGapIdFromTrigger(
  triggerEvent: HandleActionArgs["triggerEvent"]
): string | null {
  if (!triggerEvent) return null;
  if (
    triggerEvent.kind !== "DesignGapDetected" &&
    triggerEvent.kind !== "DesignGapUpdated"
  ) {
    return null;
  }
  const payload = triggerEvent.payload;
  if (!payload || typeof payload !== "object") return null;
  const id = (payload as { id?: unknown }).id;
  return typeof id === "string" && id.length > 0 ? id : null;
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
