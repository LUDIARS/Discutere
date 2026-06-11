/**
 * 常駐ワーカーの spawn。
 *
 * `lictor <bin> --model <model> [--permission-mode acceptEdits]` を起動する。
 * Concordia は無効化する (`LICTOR_DISABLE_CONCORDIA=1`) ので:
 *   - Concordia セッション登録なし → Concordia の Discord にチャンネルが作られない
 *   - permission-hook も付かない
 * claude ワーカーは auto-mode ではなく **edit-mode (`acceptEdits`)** で起動し、
 * cwd を専用ディレクトリ (`cfg.workerCwd` = worker-home) にする。そこの
 * `.claude/settings.json` で register/send スクリプトを allow-list 済みなので、
 * 旧来の生 curl が auto-mode 分類器に遮断される問題を回避しつつ待ち無しで通る。
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
  /** ワーカーの cwd (= worker-home)。専用 `.claude/settings.json` がここを基準に効く。 */
  cwd: string;
  cfg: WorkerPoolConfig;
}): SpawnedWorker {
  const { worker, promptPath, cwd, cfg } = args;
  const bin = BIN_BY_PROVIDER[worker.provider] ?? "claude";

  // claude は edit-mode (acceptEdits) で起動。worker-home の settings.json と
  // 合わせて register/send スクリプトを待ち無しで実行させる (codex は claude 専用
  // flag なので付けない)。
  const lictorArgs =
    worker.provider === "claude"
      ? [bin, "--model", worker.model, "--permission-mode", "acceptEdits"]
      : [bin, "--model", worker.model];

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
    ? spawn("cmd.exe", ["/c", "lictor", ...lictorArgs], {
        cwd,
        env,
        detached: true,
        stdio: "ignore",
        windowsHide: true,
      })
    : spawn("lictor", lictorArgs, {
        cwd,
        env,
        detached: true,
        stdio: "ignore",
      });
  child.unref();
  return { pid: child.pid ?? null };
}

/**
 * spawn した子プロセスツリーを kill する (best-effort)。
 *
 * Windows では cmd.exe /c で起動した場合 cmd.exe が launcher として即 exit し、
 * 実際に常駐するのは lictor node.exe とその ConPTY 孫 (claude.exe) である。
 * `taskkill /PID cmdPid /T /F` は cmd.exe が既に exit した後は空振りするため、
 * register 時に受け取った lictorPid (= lictor node.exe の PID) を優先して kill する。
 * cmdPid は lictorPid が得られなかった環境向けのフォールバック。
 */
export function killWorker(cmdPid: number | null, lictorPid?: number | null): void {
  const pids = [lictorPid, cmdPid].filter((p): p is number => !!p);
  if (!pids.length) return;
  for (const pid of pids) {
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
}
