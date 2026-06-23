import { spawn } from "node:child_process";

export interface GcloudSecretRequest {
  secret: string;
  version?: string;
  project?: string;
}

export function readGcloudSecret(req: GcloudSecretRequest): Promise<string> {
  const secret = req.secret.trim();
  if (!secret) return Promise.reject(new Error("gcloud secret name is required"));
  const args = ["secrets", "versions", "access", req.version || "latest", "--secret", secret];
  if (req.project?.trim()) args.push("--project", req.project.trim());

  return new Promise((resolve, reject) => {
    const child = spawn("gcloud", args, {
      cwd: process.cwd(),
      stdio: ["ignore", "pipe", "pipe"],
      windowsHide: true,
    });

    let stdout = "";
    let stderr = "";
    child.stdout.setEncoding("utf8");
    child.stderr.setEncoding("utf8");
    child.stdout.on("data", (chunk) => {
      stdout += chunk;
    });
    child.stderr.on("data", (chunk) => {
      stderr += chunk;
    });
    child.on("error", reject);
    child.on("close", (code) => {
      if (code === 0) {
        resolve(stdout.replace(/[\r\n]+$/u, ""));
        return;
      }
      reject(new Error(`gcloud exited with code ${code}: ${stderr.trim() || "(no stderr)"}`));
    });
  });
}
