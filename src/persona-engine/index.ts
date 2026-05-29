/**
 * persona-engine — public API (Phase 0)。
 *
 * 切り出し時はこの index.ts が package の entry point になる。
 * 内部実装 (db/, engine/, llm/, seeds/) は consumer から直接 import しない。
 */

import type Database from "better-sqlite3";

import { applyPersonaEngineMigrations } from "./db/migrations.js";
import { PersonasRepo } from "./db/personas-repo.js";
import { RulesRepo } from "./db/rules-repo.js";
import { createEngine, type EngineHandle, type EngineDeps } from "./engine/engine.js";
import { DISCUSSION_PERSONA_SEEDS } from "./seeds/personas.js";
import { DISCUSSION_RULE_SEEDS } from "./seeds/rules.js";
import type { LLMClient } from "./llm/client.js";
import type { DiscussionContextProvider } from "./context-provider.js";
import type { Logger, PersonaSeed, RuleSeed } from "./types.js";

export interface PersonaEngineOptions {
  db: Database.Database;
  llm: LLMClient;
  contextProvider: DiscussionContextProvider;
  logger?: Logger;
  workspaceId: string;
  /** false で seed を入れない (テスト用) */
  seedDefaults?: boolean;
  personaSeeds?: PersonaSeed[];
  ruleSeeds?: RuleSeed[];
  tickMs?: number;
  enableLlm?: boolean;
  rulesDisabled?: () => boolean;
  resolveSessionId?: EngineDeps["resolveSessionId"];
  /** Safety: 同一 session 内総発火上限 (turn budget) */
  maxFiresPerSession?: number;
  /** Safety: 同一 session 内同一 rule 発火上限 (無限ループ防止) */
  maxFiresPerRulePerSession?: number;
}

export interface PersonaEngineHandle extends EngineHandle {
  personas: PersonasRepo;
  rules: RulesRepo;
}

export function createPersonaEngine(
  opts: PersonaEngineOptions
): PersonaEngineHandle {
  applyPersonaEngineMigrations(opts.db);

  const personas = new PersonasRepo(opts.db);
  const rules = new RulesRepo(opts.db);
  const logger = opts.logger ?? consoleLogger();

  if (opts.seedDefaults !== false) {
    personas.bulkSeed(opts.personaSeeds ?? DISCUSSION_PERSONA_SEEDS);
    rules.bulkSeed(opts.ruleSeeds ?? DISCUSSION_RULE_SEEDS);
  }

  const engine = createEngine({
    personas,
    rules,
    llm: opts.llm,
    contextProvider: opts.contextProvider,
    logger,
    workspaceId: opts.workspaceId,
    tickMs: opts.tickMs,
    enableLlm: opts.enableLlm,
    rulesDisabled: opts.rulesDisabled,
    resolveSessionId: opts.resolveSessionId,
    maxFiresPerSession: opts.maxFiresPerSession,
    maxFiresPerRulePerSession: opts.maxFiresPerRulePerSession,
  });

  return Object.assign(engine, { personas, rules });
}

function consoleLogger(): Logger {
  return {
    debug: () => {},
    info: (m, msg) => console.log(`[persona-engine] ${msg}`, m),
    warn: (m, msg) => console.warn(`[persona-engine] ${msg}`, m),
    error: (m, msg) => console.error(`[persona-engine] ${msg}`, m),
  };
}

// ─── re-exports (public) ──────────────────────────
export { applyPersonaEngineMigrations } from "./db/migrations.js";
export { DISCUSSION_PERSONA_SEEDS } from "./seeds/personas.js";
export { DISCUSSION_RULE_SEEDS } from "./seeds/rules.js";
export type {
  LLMClient,
  LLMInvokeArgs,
  LLMResult,
} from "./llm/client.js";
export { AnthropicSdkClient } from "./llm/anthropic.js";
export { MockLLMClient } from "./llm/mock.js";
export type {
  DiscussionContextProvider,
  ContextHypothesis,
  ContextGap,
  ContextUtterance,
  ProposeHypothesisInput,
  PostUtteranceInput,
} from "./context-provider.js";
export type {
  PersonaSeed,
  RuleSeed,
  PersonaRow,
  PersonaAssignmentRow,
  PersonaFeedbackRow,
  RuleRow,
  RuleLogRow,
  RuleTriggerType,
  Logger,
} from "./types.js";
export { PersonasRepo } from "./db/personas-repo.js";
export { RulesRepo } from "./db/rules-repo.js";
