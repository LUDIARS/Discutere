/**
 * 常駐ワーカーの spawn。
 *
 * `lictor <bin> --model <model>` を起動する。Concordia は無効化する
 * (`LICTOR_DISABLE_CONCORDIA=1`) ので:
 *   - Concordia セッション登録なし → Concordia の Discord にチャンネルが作られない
 *   - permission-hook も付かない → ワーカーの curl callback が待ち無しで通る
 * standing persona prompt は `CONCORDIA_DELEGATION_PROMPT_FILE` 経由で Lictor の
 * delegation auto-inject が 1 回だけ流し込む。
 */

import { spawn } from "node:child_process";
import { existsSync } from "node:fs";

import type { WorkerConfig, WorkerPoolConfig } from "./types.js";

const BIN_BY_PROVIDER: Record<string, string> = {
  claude: "claude",
  codex: "codex",
};

/** git-bash パス解決 (config 明示 → 既知候補の自動検出)。 */
export function resolveGitBash(explicit?: string): string | undefined {
  if (explicit && existsSync(explicit)) return explicit;
  const user = process.env.USERNAME ?? process.env.USER ?? "";
  const cands = [
    "C:\\Program Files\\Git\\bin\\bash.exe",
    "C:\\Program Files (x86)\\Git\\bin\\bash.exe",
    `C:\\Users\\${user}\\AppData\\Local\\Atlassian\\SourceTree\\git_local\\bin\\bash.exe`,
  ];
  for (const c of cands) if (existsSync(c)) return c;
  return undefined;
}

export interface SpawnedWorker {
  pid: number | null;
}

/** 1 ワーカーを spawn し、子プロセス pid を返す (port は worker 自己 register で後から判明)。 */
export function spawnWorker(args: {
  worker: WorkerConfig;
  promptPath: string;
  cwd: string;
  cfg: WorkerPoolConfig;
}): SpawnedWorker {
  const { worker, promptPath, cwd, cfg } = args;
  const bin = BIN_BY_PROVIDER[worker.provider] ?? "claude";

  const env: NodeJS.ProcessEnv = {
    ...process.env,
    CONCORDIA_DELEGATION_PROMPT_FILE: promptPath,
    LICTOR_DELEGATION_INJECT_DELAY_MS: String(cfg.injectDelayMs),
    LICTOR_DISABLE_CONCORDIA: "1",
    DI_WORKER_ID: worker.id,
    DI_CALLBACK_URL: cfg.callbackBaseUrl,
  };
  const gb = resolveGitBash(cfg.gitBashPath);
  if (gb) env.CLAUDE_CODE_GIT_BASH_PATH = gb;
  // 親 (= この Discutere を起動した Lictor/Concordia) のラップ情報を子に持ち込まない。
  delete env.CONCORDIA_HOOK;
  delete env.LICTOR_PORT;
  delete env.LICTOR_PID;
  delete env.LICTOR_SESSION_ID;
  delete env.CONCORDIA_SESSION_ID;

  const isWin = process.platform === "win32";
  // Windows: lictor は .cmd shim なので cmd.exe 経由。detached + stdio ignore で
  // headless 常駐 (claude TUI は node-pty が pseudo-tty を確保するので可視窓は不要)。
  const child = isWin
    ? spawn("cmd.exe", ["/c", "lictor", bin, "--model", worker.model], {
        cwd,
        env,
        detached: true,
        stdio: "ignore",
        windowsHide: true,
      })
    : spawn("lictor", [bin, "--model", worker.model], {
        cwd,
        env,
        detached: true,
        stdio: "ignore",
      });
  child.unref();
  return { pid: child.pid ?? null };
}

/** spawn した子プロセスツリーを kill する (best-effort)。 */
export function killWorker(pid: number | null): void {
  if (!pid) return;
  try {
    if (process.platform === "win32") {
      spawn("taskkill", ["/PID", String(pid), "/T", "/F"], {
        stdio: "ignore",
        windowsHide: true,
      }).unref();
    } else {
      process.kill(-pid, "SIGTERM");
    }
  } catch {
    /* best-effort */
  }
}
