/**
 * persona-engine rule engine (Phase 0)。
 *
 * - 1 秒 tick + event 配信 (eventBus 不要、 fireEvent() で外部から駆動)
 * - 並列禁止 (mutex 1 つ)、 走行中の発火は skip ログ
 * - cooldown_sec 内の rule は skip (last_fired_at で判定)
 * - runtime kill switch (rulesDisabled())
 *
 * LLM 呼び出しは LLMClient 経由、 議論コンテキストは DiscussionContextProvider 経由。
 */

import type {
  DiscussionContextProvider,
} from "../context-provider.js";
import type { PersonasRepo } from "../db/personas-repo.js";
import type { RulesRepo } from "../db/rules-repo.js";
import { InMemorySessionFireStore, type SessionFireStore } from "../db/session-fires-repo.js";
import type { LLMClient } from "../llm/client.js";
import type { Logger, RuleRow } from "../types.js";
import { buildPrompt } from "./prompt-builder.js";
import { handleAction } from "./handler.js";

export interface EngineDeps {
  personas: PersonasRepo;
  rules: RulesRepo;
  llm: LLMClient;
  contextProvider: DiscussionContextProvider;
  logger: Logger;
  /** 議論ペルソナを所属させる workspace_id */
  workspaceId: string;
  /** event 発火時に渡される sessionId 解決関数 (省略時 null) */
  resolveSessionId?: (event: { kind: string; payload?: unknown }) => string | null;
  /** Runtime kill switch */
  rulesDisabled?: () => boolean;
  /** tick の周期 ms (default 1000) */
  tickMs?: number;
  /** false にすると LLM 呼ばずに skip ログ (テスト用) */
  enableLlm?: boolean;
  /**
   * Safety cap — 同一 session 内の総発火数上限。
   * 超過したら以降の fire は "skip / session cap reached" でスキップ。
   * `resetSession(sessionId)` で reset 可。
   * 未指定なら上限なし。
   */
  maxFiresPerSession?: number;
  /**
   * Safety cap — 同一 session 内で同一 rule の発火数上限。
   * 無限ループ防止 (event 連鎖で同 rule が再発火するケース) の対策。
   * 未指定なら上限なし。
   */
  maxFiresPerRulePerSession?: number;
  /**
   * per-session fire counter の保存先。未指定なら in-memory (従来挙動)。
   * SqliteSessionFireStore を渡すと再起動越しに cap が効く (restart-recovery)。
   */
  sessionFireStore?: SessionFireStore;
}

export interface EngineHandle {
  start(): void;
  stop(): void;
  /** 1 件 manual fire (テスト / debug) */
  fireRule(ruleId: string, triggeredBy: string, sessionId?: string | null): Promise<void>;
  /** 外部 event 通知 (event-driven rules を発火) */
  fireEvent(event: { kind: string; sessionId?: string | null; payload?: unknown }): Promise<void>;
  setRulesEnabled(enabled: boolean): void;
  /** session カウンタを reset (= 議論クローズ後の cap 解放) */
  resetSession(sessionId: string): void;
}

const DEFAULT_TICK_MS = 1000;

export function createEngine(deps: EngineDeps): EngineHandle {
  let timer: NodeJS.Timeout | null = null;
  let running = false;
  let stopped = false;
  let runtimeEnabled = true;
  const tickMs = deps.tickMs ?? DEFAULT_TICK_MS;

  // per-session safety counter。sessionId が null の rule (tick rule) はカウント
  // しない (cooldown で十分)。store は in-memory (既定) か sqlite (restart-recovery)。
  const sessionFires: SessionFireStore = deps.sessionFireStore ?? new InMemorySessionFireStore();

  function isDisabled(): boolean {
    if (!runtimeEnabled) return true;
    if (deps.rulesDisabled && deps.rulesDisabled()) return true;
    return false;
  }

  function nowSec(): number {
    return Math.floor(Date.now() / 1000);
  }

  function checkSessionCap(rule: RuleRow, sessionId: string | null): { ok: true } | { ok: false; reason: string } {
    if (!sessionId) return { ok: true };
    if (deps.maxFiresPerSession !== undefined) {
      const totalFires = sessionFires.total(sessionId);
      if (totalFires >= deps.maxFiresPerSession) {
        return { ok: false, reason: `session cap reached: ${totalFires}/${deps.maxFiresPerSession} (turn budget)` };
      }
    }
    if (deps.maxFiresPerRulePerSession !== undefined) {
      const perRuleCount = sessionFires.ruleCount(sessionId, rule.id);
      if (perRuleCount >= deps.maxFiresPerRulePerSession) {
        return { ok: false, reason: `rule cap reached: ${rule.id} ${perRuleCount}/${deps.maxFiresPerRulePerSession} (loop guard)` };
      }
    }
    return { ok: true };
  }

  function recordSessionFire(rule: RuleRow, sessionId: string | null): void {
    if (!sessionId) return;
    sessionFires.increment(sessionId, rule.id);
  }

  async function tryFire(
    rule: RuleRow,
    triggeredBy: string,
    sessionId: string | null,
    triggerEvent?: { kind: string; payload?: unknown }
  ): Promise<void> {
    if (stopped) return;
    if (running) {
      deps.rules.log({
        rule_id: rule.id,
        action: "skip",
        actor: "engine",
        detail: "engine busy",
      });
      return;
    }
    if (isDisabled()) {
      deps.rules.log({
        rule_id: rule.id,
        action: "skip",
        actor: "engine",
        detail: "rules disabled at runtime",
      });
      return;
    }
    const now = nowSec();
    if (rule.last_fired_at && now - rule.last_fired_at < rule.cooldown_sec) {
      return; // cooldown 内、 ログも spam なので無音
    }

    const cap = checkSessionCap(rule, sessionId);
    if (!cap.ok) {
      deps.rules.log({
        rule_id: rule.id,
        action: "skip",
        actor: "engine",
        detail: cap.reason,
      });
      return;
    }

    running = true;
    try {
      deps.rules.setLastFired(rule.id, now);
      recordSessionFire(rule, sessionId);
      await fireOnce(rule, triggeredBy, sessionId, triggerEvent);
    } catch (err) {
      const message = (err as Error).message ?? String(err);
      deps.rules.log({
        rule_id: rule.id,
        action: "error",
        actor: "engine",
        detail: message,
      });
      deps.logger.warn({ rule_id: rule.id, err: message }, "fire failed");
    } finally {
      running = false;
    }
  }

  async function fireOnce(
    rule: RuleRow,
    triggeredBy: string,
    sessionId: string | null,
    triggerEvent?: { kind: string; payload?: unknown }
  ): Promise<void> {
    const personaId = rule.target;
    if (!personaId) {
      deps.rules.log({
        rule_id: rule.id,
        action: "skip",
        actor: "engine",
        detail: "rule.target persona_id not set",
      });
      return;
    }
    const persona = deps.personas.get(personaId);
    if (!persona) {
      deps.rules.log({
        rule_id: rule.id,
        action: "error",
        actor: "engine",
        detail: `persona not found: ${personaId}`,
      });
      return;
    }

    const built = buildPrompt({
      rule,
      persona,
      workspaceId: deps.workspaceId,
      sessionId,
      contextProvider: deps.contextProvider,
      triggeredBy,
    });

    if (deps.enableLlm === false) {
      deps.rules.log({
        rule_id: rule.id,
        action: "skip",
        actor: "engine",
        detail: "llm disabled in deps",
      });
      return;
    }

    const llmRes = await deps.llm.invoke({
      prompt: built.user,
      system: built.system,
    });
    if (!llmRes.ok) {
      deps.rules.log({
        rule_id: rule.id,
        action: "error",
        actor: "engine",
        detail: `llm error: ${llmRes.error}`,
      });
      deps.logger.warn(
        { rule_id: rule.id, err: llmRes.error },
        "llm invoke failed"
      );
      return;
    }

    handleAction({
      ruleId: rule.id,
      personaId,
      workspaceId: deps.workspaceId,
      sessionId,
      triggerEvent,
      rawText: llmRes.text,
      contextProvider: deps.contextProvider,
      rules: deps.rules,
      logger: deps.logger,
    });
  }

  return {
    start(): void {
      if (timer) return;
      stopped = false;
      timer = setInterval(async () => {
        if (stopped) return;
        const ticks = deps.rules.list({ enabled: true, trigger_type: "tick" });
        const now = nowSec();
        for (const r of ticks) {
          if (!r.tick_sec) continue;
          const elapsed = r.last_fired_at ? now - r.last_fired_at : Infinity;
          if (elapsed >= r.tick_sec) {
            await tryFire(r, "tick", null);
          }
        }
      }, tickMs);
      deps.logger.info({ tickMs }, "persona-engine started");
    },
    stop(): void {
      stopped = true;
      if (timer) {
        clearInterval(timer);
        timer = null;
      }
      deps.logger.info({}, "persona-engine stopped");
    },
    async fireRule(ruleId, triggeredBy, sessionId = null): Promise<void> {
      const r = deps.rules.get(ruleId);
      if (!r) {
        deps.logger.warn({ rule_id: ruleId }, "fireRule: rule not found");
        return;
      }
      await tryFire(r, triggeredBy, sessionId ?? null);
    },
    async fireEvent(event): Promise<void> {
      const sessionId = event.sessionId ?? (deps.resolveSessionId?.(event) ?? null);
      const candidates = deps.rules
        .list({ enabled: true, trigger_type: "event" })
        .filter((r) => !r.event_kind || r.event_kind === event.kind);
      for (const r of candidates) {
        await tryFire(r, `event:${event.kind}`, sessionId, {
          kind: event.kind,
          payload: event.payload,
        });
      }
    },
    setRulesEnabled(enabled: boolean): void {
      runtimeEnabled = enabled;
      deps.logger.info({ enabled }, "rules runtime switch");
    },
    resetSession(sessionId: string): void {
      sessionFires.reset(sessionId);
      deps.logger.info({ sessionId }, "session counter reset");
    },
  };
}
