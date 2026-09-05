/**
 * CodexCliClient — `codex exec` (非対話) を 1 発話ごとに spawn する LLMClient。
 *
 * Sol / Terra / Luna (gpt-5.6-*) をサブスクの Codex CLI 経由で議論に参加させる。
 * Anthropic 経路 (claude-cli / anthropic) と同じ `LLMClient` 契約に収め、
 * model spec の `@<effort>` を `-c model_reasoning_effort=<effort>` に落とす。
 *
 * - prompt は stdin で渡す (Windows の引数長・コードページ変換を踏まない)。
 * - 最終メッセージは `-o <file>` でファイルに書かせて読む (stdout のイベント混入を避ける)。
 * - sandbox は read-only、`--ephemeral` でセッションを残さない、cwd は worker-home。
 */

import { spawn } from "node:child_process";
import { mkdtempSync, readFileSync, rmSync, statSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

import type { LLMClient, LLMInvokeArgs, LLMResult } from "./client.js";
import { logLlm } from "./llm-vg.js";
import { parseModelSpec, validateModelSpec } from "./model-spec.js";

export interface CodexCliClientOptions {
  /** codex CLI のパス (PATH に乗っていれば不要)。 */
  cliPath?: string;
  /** 既定 model spec (`gpt-5.6-sol@medium` 等)。invoke の model が優先。 */
  defaultModel?: string;
  /** タイムアウト ms (既定 240_000。xhigh は長い)。 */
  defaultTimeoutMs?: number;
  /** codex を起動する cwd (既定 process.cwd())。 */
  cwd?: string;
}

const DEFAULT_TIMEOUT_MS = 240_000;
const MAX_RESPONSE_BYTES = 1_048_576;
const CODEX_ENV_ALLOWLIST = [
  "PATH",
  "Path",
  "PATHEXT",
  "SystemRoot",
  "WINDIR",
  "COMSPEC",
  "TEMP",
  "TMP",
  "HOME",
  "USERPROFILE",
  "APPDATA",
  "LOCALAPPDATA",
  "CODEX_HOME",
  "LANG",
  "LC_ALL",
] as const;

function errorCode(error: unknown): string {
  const code = (error as NodeJS.ErrnoException | undefined)?.code;
  return code ? ` (${code})` : "";
}

/** @implements SPEC-FLOW-MODEL-ROSTER — サービス資格情報を Codex 子プロセスへ継承しない。 */
export function buildCodexEnvironment(source: NodeJS.ProcessEnv): NodeJS.ProcessEnv {
  const env: NodeJS.ProcessEnv = {};
  for (const key of CODEX_ENV_ALLOWLIST) {
    if (source[key] !== undefined) env[key] = source[key];
  }
  return env;
}

function composePrompt(system: string | undefined, user: string): string {
  if (!system) return user;
  return `[system]\n${system}\n\n[user]\n${user}`;
}

export class CodexCliClient implements LLMClient {
  private readonly cliPath: string;
  private readonly defaultModel?: string;
  private readonly defaultTimeoutMs: number;
  private readonly cwd?: string;

  constructor(options: CodexCliClientOptions = {}) {
    this.cliPath = options.cliPath ?? "codex";
    this.defaultModel = options.defaultModel;
    this.defaultTimeoutMs = options.defaultTimeoutMs ?? DEFAULT_TIMEOUT_MS;
    this.cwd = options.cwd;
  }

  async invoke(args: LLMInvokeArgs): Promise<LLMResult> {
    const spec = parseModelSpec(args.model ?? this.defaultModel);
    const validationError = validateModelSpec(spec, "codex");
    if (validationError) return { ok: false, error: `codex-cli: ${validationError}` };
    const timeoutMs = args.timeoutMs ?? this.defaultTimeoutMs;
    const prompt = composePrompt(args.system, args.prompt);
    let workDir: string;
    try {
      workDir = mkdtempSync(join(tmpdir(), "di-codex-"));
    } catch (e) {
      return { ok: false, error: `codex-cli: 一時領域を作成できません${errorCode(e)}` };
    }
    const outFile = join(workDir, "last-message.txt");

    const cliArgs = [
      "exec",
      "-m",
      spec.model,
      ...(spec.effort ? ["-c", `model_reasoning_effort="${spec.effort}"`] : []),
      "-s",
      "read-only",
      "--ephemeral",
      "--skip-git-repo-check",
      "--color",
      "never",
      "-o",
      outFile,
      "-",
    ];

    try {
      const result = await new Promise<LLMResult>((resolve) => {
        let child;
        try {
          child = spawn(this.cliPath, cliArgs, {
            cwd: this.cwd,
            env: buildCodexEnvironment(process.env),
            // npm の Windows shim (.cmd) を起動するため shell が必要。model / effort は
            // validateModelSpec で shell メタ文字を拒否してから argv に入れる。
            shell: process.platform === "win32",
            windowsHide: true,
            stdio: ["pipe", "pipe", "pipe"],
          });
        } catch (e) {
          resolve({ ok: false, error: `codex cli spawn failed${errorCode(e)}` });
          return;
        }
        let resolved = false;
        let timedOut = false;
        let killFallback: NodeJS.Timeout | undefined;
        const finish = (value: LLMResult): void => {
          if (resolved) return;
          resolved = true;
          clearTimeout(timer);
          if (killFallback) clearTimeout(killFallback);
          resolve(value);
        };
        const timer = setTimeout(() => {
          timedOut = true;
          try {
            child.kill("SIGKILL");
          } catch {
            finish({ ok: false, error: `codex cli timeout (${timeoutMs}ms)` });
            return;
          }
          // 通常は close で完了する。OS が close を通知しない場合だけ解放待ちを打ち切る。
          killFallback = setTimeout(
            () => finish({ ok: false, error: `codex cli timeout (${timeoutMs}ms)` }),
            5_000
          );
        }, timeoutMs);
        child.stdout?.on("data", () => { /* イベント出力は捨てる (最終文は -o で読む) */ });
        child.stderr?.on("data", () => { /* ローカルパス等を上位へ漏らさないため診断出力は捨てる */ });
        child.on("error", (e) => finish({ ok: false, error: `codex cli error${errorCode(e)}` }));
        child.on("close", (code) => {
          if (resolved) return;
          if (timedOut) {
            finish({ ok: false, error: `codex cli timeout (${timeoutMs}ms)` });
            return;
          }
          if (code !== 0) {
            finish({ ok: false, error: `codex cli exit ${code}` });
            return;
          }
          let text = "";
          try {
            if (statSync(outFile).size > MAX_RESPONSE_BYTES) {
              finish({ ok: false, error: `codex cli: response exceeds ${MAX_RESPONSE_BYTES} bytes` });
              return;
            }
            text = readFileSync(outFile, "utf8").trim();
          } catch { /* 未生成 */ }
          finish(text ? { ok: true, text } : { ok: false, error: "codex cli: empty response" });
        });
        child.stdin?.on("error", () => { /* EPIPE 等は close 側で拾う */ });
        child.stdin?.end(prompt, "utf8");
      });

      logLlm({
        backend: "codex-cli",
        model: spec.effort ? `${spec.model}@${spec.effort}` : spec.model,
        system: args.system,
        prompt: args.prompt,
        ok: result.ok,
        error: result.ok ? undefined : result.error,
      });
      return result;
    } finally {
      try { rmSync(workDir, { recursive: true, force: true }); } catch { /* best-effort cleanup */ }
    }
  }
}
