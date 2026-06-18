/**
 * Runtime 設定ストア — boot 時 hardcoded をデフォルトに、 SQLite override があれば
 * 差し替えて返す。 facilitator のトリガールール (収束しきい値) / ワーカーの役割
 * プロンプト / debate ルール instructions を、 再起動なしで Web UI から調整するための
 * 単一の永続レイヤー。
 *
 * - 保存先は persona-engine の SQLite (peDb) に同居する `runtime_settings` テーブル。
 * - 値は JSON 文字列。 key 体系:
 *     facilitator             → FacilitatorTuning の部分 override (JSON)
 *     rolePrompt:<role>       → 役割ガイダンス本文 (string)
 *     ruleInstruction:<id>    → debate ルール instructions 本文 (string)
 * - in-memory cache を持ち、 set 時に更新。 別プロセスが書く想定は無い (単一 bot)。
 */

import type DatabaseType from "better-sqlite3";

export type ConvergePolicy = "default" | "aufhebung-only";

/** facilitator の収束トリガー設定。 「20 意見で収束」= maxPersonas。 */
export interface FacilitatorTuning {
  /** tick 周期 ms (start 時に固定。 変更は次回 start で反映)。 */
  tickMs: number;
  /** 発言が止まってから介入するまでの idle ms。 */
  idleGapMs: number;
  /** 参加 persona がこの数を超えたら収束 (= ご指摘の「20」)。 */
  maxPersonas: number;
  /** 止揚 (アウフヘーベン) がこの数たまったら収束。 */
  aufhebungTarget: number;
  /**
   * 収束ポリシー:
   *  - "default"        : 止揚 N 件 または persona 数超過 で収束 (従来動作)。
   *  - "aufhebung-only" : persona 数では収束せず、 止揚が出るまで議論を続ける。
   */
  convergePolicy: ConvergePolicy;
}

/**
 * コスト relay 設定 (Anatomia / Concordia への push)。再起動なしで Web UI から
 * 有効化・送信先を編集するため runtime-settings に持つ。`intervalMs` (timer 周期)
 * は startCostRelay 起動時に固定するので含めない (変更は再起動)。
 */
export interface CostRelaySettings {
  /** 定期 relay を有効化するか。 */
  enabled: boolean;
  /** Anatomia ベース URL (空 = 送らない)。POST /api/cost-feed。 */
  anatomiaUrl: string;
  /** Concordia ベース URL (空 = 送らない)。POST /v1/cost-feed。 */
  concordiaUrl: string;
  /** cost-feed の service ラベル。 */
  service: string;
}

/** hardcoded デフォルト群 (config / 各 seed から注入)。 */
export interface RuntimeSettingsDefaults {
  facilitator: FacilitatorTuning;
  /** 役割名 → 既定ガイダンス本文。 */
  rolePrompts: Record<string, string>;
  /** ルール id → 既定 instructions 本文。 */
  ruleInstructions: Record<string, string>;
  /** コスト relay の既定 (config 由来)。 */
  costRelay: CostRelaySettings;
}

/** UI 表示用: デフォルト / override / 実効値 の 3 点セット。 */
export interface OverrideView {
  key: string;
  default: string;
  override: string | null;
  effective: string;
}

export interface RuntimeSettingsStore {
  /** facilitator の実効チューニング (デフォルト + override)。 */
  getFacilitatorTuning(): FacilitatorTuning;
  /** facilitator override を部分更新して永続化。 実効値を返す。 */
  setFacilitatorTuning(patch: Partial<FacilitatorTuning>): FacilitatorTuning;

  /** 役割の実効ガイダンス (override 優先、 無ければデフォルト)。 */
  getRolePrompt(role: string): string;
  /** 全役割の default/override/effective 一覧。 */
  listRolePrompts(): OverrideView[];
  /** 役割 override を設定。 null/空文字でデフォルトに戻す。 */
  setRolePrompt(role: string, text: string | null): void;

  /** ルールの実効 instructions (override 優先)。 */
  getRuleInstruction(ruleId: string): string;
  /** 全ルールの default/override/effective 一覧。 */
  listRuleInstructions(): OverrideView[];
  /** ルール instructions override を設定。 null/空文字でデフォルトに戻す。 */
  setRuleInstruction(ruleId: string, text: string | null): void;

  /** コスト relay の実効設定 (デフォルト + override)。 */
  getCostRelay(): CostRelaySettings;
  /** コスト relay override を部分更新して永続化。 実効値を返す。 */
  setCostRelay(patch: Partial<CostRelaySettings>): CostRelaySettings;

  /** DB から cache を読み直す。 */
  reload(): void;
}

const FACILITATOR_KEY = "facilitator";
const ROLE_PREFIX = "rolePrompt:";
const RULE_PREFIX = "ruleInstruction:";
const COST_RELAY_KEY = "costRelay";

/**
 * cost relay override (部分) をデフォルトにマージして実効設定を作る (pure)。
 * URL は trim、文字列以外はデフォルトに落とす。
 */
export function mergeCostRelay(
  def: CostRelaySettings,
  override: Partial<CostRelaySettings> | null,
): CostRelaySettings {
  if (!override) return { ...def };
  const str = (v: unknown, fallback: string): string =>
    typeof v === "string" ? v.trim() : fallback;
  return {
    enabled: typeof override.enabled === "boolean" ? override.enabled : def.enabled,
    anatomiaUrl: str(override.anatomiaUrl, def.anatomiaUrl),
    concordiaUrl: str(override.concordiaUrl, def.concordiaUrl),
    service: str(override.service, def.service) || def.service,
  };
}

function clampInt(v: unknown, min: number, fallback: number): number {
  const n = typeof v === "number" ? v : Number(v);
  if (!Number.isFinite(n)) return fallback;
  return Math.max(min, Math.round(n));
}

function normalizePolicy(v: unknown, fallback: ConvergePolicy): ConvergePolicy {
  return v === "default" || v === "aufhebung-only" ? v : fallback;
}

/**
 * facilitator override (部分 JSON) をデフォルトにマージして実効 tuning を作る (pure)。
 * 数値は下限クランプ、 policy は enum 検証。 不正値はデフォルトに落とす。
 */
export function mergeFacilitatorTuning(
  def: FacilitatorTuning,
  override: Partial<FacilitatorTuning> | null,
): FacilitatorTuning {
  if (!override) return { ...def };
  return {
    tickMs: clampInt(override.tickMs, 1_000, def.tickMs),
    idleGapMs: clampInt(override.idleGapMs, 1_000, def.idleGapMs),
    maxPersonas: clampInt(override.maxPersonas, 1, def.maxPersonas),
    aufhebungTarget: clampInt(override.aufhebungTarget, 1, def.aufhebungTarget),
    convergePolicy: normalizePolicy(override.convergePolicy, def.convergePolicy),
  };
}

/**
 * peDb 上に `runtime_settings` を確保して store を構築する。
 * defaults は呼び出し側 (index.ts) が config / 各 seed から組み立てて渡す。
 */
export function createRuntimeSettingsStore(
  db: DatabaseType.Database,
  defaults: RuntimeSettingsDefaults,
): RuntimeSettingsStore {
  db.exec(
    `CREATE TABLE IF NOT EXISTS runtime_settings (
       key        TEXT PRIMARY KEY,
       value      TEXT NOT NULL,
       updated_at INTEGER NOT NULL
     )`,
  );

  const cache = new Map<string, string>();
  function loadAll(): void {
    cache.clear();
    const rows = db.prepare("SELECT key, value FROM runtime_settings").all() as Array<{
      key: string;
      value: string;
    }>;
    for (const r of rows) cache.set(r.key, r.value);
  }
  loadAll();

  function put(key: string, value: string, now: number): void {
    db.prepare(
      `INSERT INTO runtime_settings (key, value, updated_at) VALUES (?, ?, ?)
         ON CONFLICT(key) DO UPDATE SET value = excluded.value, updated_at = excluded.updated_at`,
    ).run(key, value, now);
    cache.set(key, value);
  }
  function del(key: string): void {
    db.prepare("DELETE FROM runtime_settings WHERE key = ?").run(key);
    cache.delete(key);
  }

  function parseFacilitatorOverride(): Partial<FacilitatorTuning> | null {
    const raw = cache.get(FACILITATOR_KEY);
    if (!raw) return null;
    try {
      return JSON.parse(raw) as Partial<FacilitatorTuning>;
    } catch {
      return null;
    }
  }

  function parseCostRelayOverride(): Partial<CostRelaySettings> | null {
    const raw = cache.get(COST_RELAY_KEY);
    if (!raw) return null;
    try {
      return JSON.parse(raw) as Partial<CostRelaySettings>;
    } catch {
      return null;
    }
  }

  function overrideView(key: string, def: string): OverrideView {
    const override = cache.get(key) ?? null;
    return { key, default: def, override, effective: override ?? def };
  }

  return {
    getFacilitatorTuning(): FacilitatorTuning {
      return mergeFacilitatorTuning(defaults.facilitator, parseFacilitatorOverride());
    },
    setFacilitatorTuning(patch: Partial<FacilitatorTuning>): FacilitatorTuning {
      const cur = parseFacilitatorOverride() ?? {};
      const merged = mergeFacilitatorTuning(defaults.facilitator, { ...cur, ...patch });
      // 実効値そのものを override として保存 (常に完全な tuning を持たせる)。
      put(FACILITATOR_KEY, JSON.stringify(merged), nowMs(db));
      return merged;
    },

    getRolePrompt(role: string): string {
      return cache.get(ROLE_PREFIX + role) ?? defaults.rolePrompts[role] ?? "";
    },
    listRolePrompts(): OverrideView[] {
      return Object.keys(defaults.rolePrompts).map((role) =>
        overrideView(ROLE_PREFIX + role, defaults.rolePrompts[role]),
      );
    },
    setRolePrompt(role: string, text: string | null): void {
      const key = ROLE_PREFIX + role;
      if (text === null || text.trim() === "") del(key);
      else put(key, text, nowMs(db));
    },

    getRuleInstruction(ruleId: string): string {
      return cache.get(RULE_PREFIX + ruleId) ?? defaults.ruleInstructions[ruleId] ?? "";
    },
    listRuleInstructions(): OverrideView[] {
      return Object.keys(defaults.ruleInstructions).map((id) =>
        overrideView(RULE_PREFIX + id, defaults.ruleInstructions[id]),
      );
    },
    setRuleInstruction(ruleId: string, text: string | null): void {
      const key = RULE_PREFIX + ruleId;
      if (text === null || text.trim() === "") del(key);
      else put(key, text, nowMs(db));
    },

    getCostRelay(): CostRelaySettings {
      return mergeCostRelay(defaults.costRelay, parseCostRelayOverride());
    },
    setCostRelay(patch: Partial<CostRelaySettings>): CostRelaySettings {
      const cur = parseCostRelayOverride() ?? {};
      const merged = mergeCostRelay(defaults.costRelay, { ...cur, ...patch });
      // 実効値そのものを override として保存 (常に完全な設定を持たせる)。
      put(COST_RELAY_KEY, JSON.stringify(merged), nowMs(db));
      return merged;
    },

    reload(): void {
      loadAll();
    },
  };
}

/**
 * SQLite から現在時刻を取る (テストの Date.now モック差異を避けるため SQL 側で取得)。
 * better-sqlite3 は同期なので問題ない。
 */
function nowMs(db: DatabaseType.Database): number {
  const row = db.prepare("SELECT CAST(strftime('%s','now') AS INTEGER) * 1000 AS ms").get() as {
    ms: number;
  };
  return row.ms;
}
