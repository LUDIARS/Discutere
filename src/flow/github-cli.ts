import { execFile } from "node:child_process";

export interface GhCommandResult {
  exitCode: number;
  stdout: string;
  stderr: string;
  error?: string;
}

export interface GitHubCliAuthStatus {
  available: boolean;
  authenticated: boolean;
  output: string;
}

function runGh(args: string[], timeoutMs = 5000): Promise<GhCommandResult> {
  return new Promise((resolve) => {
    const child = execFile("gh", args, { timeout: timeoutMs, windowsHide: true }, (error, stdout, stderr) => {
      const rawCode = (error as unknown as { code?: unknown } | null)?.code;
      const exitCode = typeof rawCode === "number" ? rawCode : error ? 1 : 0;
      resolve({
        exitCode,
        stdout: String(stdout ?? ""),
        stderr: String(stderr ?? ""),
        error: error ? error.message : undefined,
      });
    });
    child.stdin?.end();
  });
}

function stripAnsi(s: string): string {
  return s.replace(/\x1b\[[0-9;]*m/g, "");
}

function redactSecrets(s: string): string {
  return s
    .replace(/\bgh[pousr]_[A-Za-z0-9_]{20,}\b/g, "[redacted-token]")
    .replace(/\bgithub_pat_[A-Za-z0-9_]{20,}\b/g, "[redacted-token]");
}

export async function getGitHubCliAuthStatus(): Promise<GitHubCliAuthStatus> {
  const r = await runGh(["auth", "status", "--hostname", "github.com"]);
  const output = redactSecrets(stripAnsi(`${r.stdout}${r.stderr}`)).trim() || r.error || "gh auth status returned no output";
  return {
    available: !r.error || !/ENOENT|not recognized|not found/i.test(r.error),
    authenticated: r.exitCode === 0,
    output,
  };
}

export async function getGitHubCliToken(): Promise<string | null> {
  const r = await runGh(["auth", "token", "--hostname", "github.com"]);
  if (r.exitCode !== 0) return null;
  const token = r.stdout.trim();
  return token.length > 0 ? token : null;
}
