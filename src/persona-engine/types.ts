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
  /** プロンプトキャッシュ読込トークン (cache hit 分)。backend が返す時のみ。 */
  cache_read_input_tokens?: number;
  /** プロンプトキャッシュ書込トークン。backend が返す時のみ。 */
  cache_creation_input_tokens?: number;
  /**
   * このコールの推定コスト (USD)。
   * claude-cli (サブスク) は等価 API 換算の推定値 (実課金ではない / サブスクは定額)、
   * anthropic 従量経路では未取得 (raw API は返さない) なので通常 undefined。
   */
  cost_usd?: number;
  /**
   * このコールを実際に処理したモデル id (例 "claude-haiku-4-5-20251001")。
   * worker-pool 経路は呼び出し時の `args.model` が常に claude id とは限らない
   * (非 claude provider は undefined) ため、transcript の assistant `message.model`
   * を ground truth として載せ、cost 補完 (estimateCostUsd) がモデル単価を引けるようにする。
   */
  model?: string;
}
