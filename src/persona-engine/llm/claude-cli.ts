/**
 * Claude Code CLI subprocess wrapper (= Lictor 経由 spawn) — PR-I.
 *
 * `claude -p` を spawn して stdin に prompt を渡し、 stdout 全文を取得。
 * - LUDIARS feedback (feedback_claude_cli_long_prompt) — prompt は stdin で
 * - Windows feedback (feedback_claude_cli_windows_bash) — CLAUDE_CODE_GIT_BASH_PATH
 * - timeout (default 120s)
 *
 * Lictor は session を hold する sidecar だが、 ここでは「claude CLI を直接
 * spawn する」 形で「Lictor wrapping 配下の Claude Code」 と同等の挙動を出す。
 * 議論ループから per-fire spawn するので重い (= 1 fire = 1 process)、 別途
 * pooling 化は Phase 1 で。
 */

import { spawn } from "node:child_process";
import { existsSync } from "node:fs";

import type { LLMClient, LLMInvokeArgs, LLMResult } from "./client.js";
import type { TokenUsage } from "../types.js";

export interface ClaudeCliClientOptions {
  /** 任意 — claude CLI のフルパス (PATH に乗ってる場合は不要) */
  cliPath?: string;
  /** 任意 — model 指定 (claude -p に --model フラグで渡す予定だが、 Phase 0 では未使用) */
  defaultModel?: string;
  /** タイムアウト ms (default 120_000) */
  defaultTimeoutMs?: number;
  /** 任意 — Windows で claude CLI が要する git-bash パス (config 由来、未指定なら自動検出) */
  gitBashPath?: string;
}

const DEFAULT_TIMEOUT_MS = 120_000;

export class ClaudeCliClient implements LLMClient {
  private readonly cliPath: string;
  private readonly defaultTimeoutMs: number;
  private readonly gitBashPath?: string;
  private readonly defaultModel?: string;

  constructor(options: ClaudeCliClientOptions = {}) {
    this.cliPath = options.cliPath ?? "claude";
    this.defaultTimeoutMs = options.defaultTimeoutMs ?? DEFAULT_TIMEOUT_MS;
    this.gitBashPath = options.gitBashPath;
    this.defaultModel = options.defaultModel;
  }

  async invoke(args: LLMInvokeArgs): Promise<LLMResult> {
    const timeoutMs = args.timeoutMs ?? this.defaultTimeoutMs;
    return spawnClaude({
      cliPath: this.cliPath,
      prompt: composePrompt(args.system, args.prompt),
      timeoutMs,
      gitBashPath: this.gitBashPath,
      // delegation 準拠: per-invoke model > client 既定 model。未指定なら CLI 既定。
      model: args.model ?? this.defaultModel,
    });
  }
}

function composePrompt(system: string | undefined, user: string): string {
  if (!system) return user;
  return `[system]\n${system}\n\n[user]\n${user}`;
}

interface SpawnArgs {
  cliPath: string;
  prompt: string;
  timeoutMs: number;
  gitBashPath?: string;
  /** claude CLI に渡す --model (delegation 準拠)。未指定なら付けず CLI 既定。 */
  model?: string;
}

function spawnClaude(args: SpawnArgs): Promise<LLMResult> {
  return new Promise((resolve) => {
    const env: NodeJS.ProcessEnv = { ...process.env };
    if (args.gitBashPath) {
      env.CLAUDE_CODE_GIT_BASH_PATH = args.gitBashPath;
    }
    if (process.platform === "win32" && !env.CLAUDE_CODE_GIT_BASH_PATH) {
      const candidates = [
        "C:\\Program Files\\Git\\bin\\bash.exe",
        "C:\\Program Files (x86)\\Git\\bin\\bash.exe",
      ];
      for (const c of candidates) {
        if (existsSync(c)) {
          env.CLAUDE_CODE_GIT_BASH_PATH = c;
          break;
        }
      }
    }
    // Lictor / Concordia 連携を持ち込まないよう環境変数を絞る
    delete env.CONCORDIA_HOOK;
    delete env.LICTOR_PORT;

    // --output-format json: stdout を JSON エンベロープにし、result(本文) +
    // usage(cache_read/creation 含む) + total_cost_usd を取得できるようにする。
    // サブスク(OAuth) でも total_cost_usd は等価 API 換算で populated される。
    const cliArgs = ["-p", "--output-format", "json"];
    if (args.model) cliArgs.push("--model", args.model);
    let child;
    try {
      child = spawn(args.cliPath, cliArgs, {
        env,
        shell: process.platform === "win32",
        windowsHide: true,
      });
    } catch (e) {
      resolve({ ok: false, error: `claude cli spawn failed: ${(e as Error).message}` });
      return;
    }

    let out = "";
    let err = "";
    let resolved = false;
    const timer = setTimeout(() => {
      if (resolved) return;
      resolved = true;
      try { child.kill("SIGKILL"); } catch {}
      resolve({ ok: false, error: `claude cli timeout (${args.timeoutMs} ms)` });
    }, args.timeoutMs);

    child.stdout.on("data", (d) => { out += d.toString("utf8"); });
    child.stderr.on("data", (d) => { err += d.toString("utf8"); });
    child.on("error", (e) => {
      if (resolved) return;
      resolved = true;
      clearTimeout(timer);
      resolve({ ok: false, error: `claude cli error: ${e.message}` });
    });
    child.on("close", (code) => {
      if (resolved) return;
      resolved = true;
      clearTimeout(timer);
      if (code !== 0) {
        resolve({
          ok: false,
          error: `claude cli exit ${code}: ${err.slice(0, 200)}`,
        });
        return;
      }
      const raw = out.trim();
      if (raw.length === 0) {
        resolve({ ok: false, error: "claude cli empty output" });
        return;
      }
      resolve(parseClaudeCliResult(raw));
    });

    // stdin で prompt 渡し (LUDIARS feedback_claude_cli_long_prompt)
    child.stdin.end(args.prompt);
  });
}

/** claude -p --output-format json の result エンベロープ (必要分のみ)。 */
interface ClaudeCliResultEnvelope {
  type?: string;
  subtype?: string;
  is_error?: boolean;
  result?: string;
  total_cost_usd?: number;
  usage?: {
    input_tokens?: number;
    output_tokens?: number;
    cache_read_input_tokens?: number;
    cache_creation_input_tokens?: number;
  };
}

/**
 * `claude -p --output-format json` の stdout をパースして LLMResult にする。
 *
 * CLI のバージョンにより出力は (a) 単一の result オブジェクト か
 * (b) イベント配列 (末尾に type:"result") のどちらか。両方に対応する。
 * パース失敗・result 非 success・本文空は ok:false。
 * 純関数なのでテストから直接叩ける。
 */
export function parseClaudeCliResult(raw: string): LLMResult {
  let parsed: unknown;
  try {
    parsed = JSON.parse(raw);
  } catch (e) {
    return {
      ok: false,
      error: `claude cli json parse failed: ${(e as Error).message}: ${raw.slice(0, 200)}`,
    };
  }

  // 配列なら type:"result" の要素 (無ければ末尾) を採る。
  let env: ClaudeCliResultEnvelope | undefined;
  if (Array.isArray(parsed)) {
    const arr = parsed as ClaudeCliResultEnvelope[];
    env = arr.find((e) => e?.type === "result") ?? arr[arr.length - 1];
  } else if (parsed && typeof parsed === "object") {
    env = parsed as ClaudeCliResultEnvelope;
  }

  if (!env) {
    return { ok: false, error: "claude cli json: no result envelope" };
  }
  if (env.is_error || (env.subtype && env.subtype !== "success")) {
    return {
      ok: false,
      error: `claude cli result error (${env.subtype ?? "unknown"}): ${(env.result ?? "").slice(0, 200)}`,
    };
  }

  const text = typeof env.result === "string" ? env.result.trim() : "";
  if (text.length === 0) {
    return { ok: false, error: "claude cli json: empty result text" };
  }

  const u = env.usage ?? {};
  const usage: TokenUsage = {
    input_tokens: u.input_tokens,
    output_tokens: u.output_tokens,
    cache_read_input_tokens: u.cache_read_input_tokens,
    cache_creation_input_tokens: u.cache_creation_input_tokens,
    cost_usd: typeof env.total_cost_usd === "number" ? env.total_cost_usd : undefined,
  };
  return { ok: true, text, usage };
}
