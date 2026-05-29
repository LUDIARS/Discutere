/**
 * persona-engine 共通型定義。
 *
 * 切り出し時はこのファイルが pacakge の "main types entry" になる。
 * Discutere 固有の型 (Hypothesis / Gap / Utterance 等) は context-provider.ts に隔離する。
 */

export interface PersonaRow {
  id: string;
  name: string;
  display_name: string;
  description: string;
  /** JSON 化された string[] */
  traits: string;
  speech_style: string;
  /** future use (Concordia 互換) */
  skill_template: string;
  /** JSON 化された learning note 配列 */
  learned_notes: string;
  created_at: number;
  updated_at: number;
}

export interface PersonaAssignmentRow {
  id: number;
  persona_id: string;
  session_id: string;
  assigned_at: number;
  released_at: number | null;
}

export interface PersonaFeedbackRow {
  id: number;
  persona_id: string;
  session_id: string | null;
  ts: number;
  kind: "session-end" | "chat-update" | "manual" | "system";
  delta: string;
  detail: string | null;
}

export type RuleTriggerType = "tick" | "event";

export interface RuleRow {
  id: string;
  description: string | null;
  trigger_type: RuleTriggerType;
  tick_sec: number | null;
  event_kind: string | null;
  /** JSON 化された condition 配列 (Phase 0 では未使用 = "[]") */
  conditions: string;
  /** LLM に渡される主指示 */
  instructions: string;
  /** 任意の対象 persona_id (rule fire 時に persona を絞る) */
  target: string | null;
  cooldown_sec: number;
  last_fired_at: number | null;
  enabled: number;
  added_at: number;
  added_by: string;
  removed_at: number | null;
  removed_by: string | null;
  removed_reason: string | null;
}

export interface RuleLogRow {
  id: number;
  ts: number;
  rule_id: string | null;
  action: "add" | "remove" | "fire" | "skip" | "error";
  detail: string | null;
  actor: "system" | "ai" | "human" | "engine";
}

export interface PersonaSeed {
  id: string;
  name: string;
  display_name: string;
  description: string;
  traits: string[];
  speech_style: string;
}

export interface RuleSeed {
  id: string;
  description?: string;
  trigger_type: RuleTriggerType;
  tick_sec?: number | null;
  event_kind?: string | null;
  instructions: string;
  target?: string | null;
  cooldown_sec?: number;
}

export interface Logger {
  debug(meta: Record<string, unknown>, message: string): void;
  info(meta: Record<string, unknown>, message: string): void;
  warn(meta: Record<string, unknown>, message: string): void;
  error(meta: Record<string, unknown>, message: string): void;
}

export interface TokenUsage {
  input_tokens?: number;
  output_tokens?: number;
}
