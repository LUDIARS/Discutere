#!/usr/bin/env tsx
import { spawn } from "node:child_process";
import path from "node:path";

import envCliConfig from "../env-cli.config.js";
import { loadBootstrap } from "../../Cernere/packages/env-cli/src/env-file.js";
import { authenticate, upsertSecret } from "../../Cernere/packages/env-cli/src/infisical.js";
import type { EnvCliConfig } from "../../Cernere/packages/env-cli/src/types.js";

const DEFAULT_GCLOUD_SECRET = "discutere-youtube-api-key";
const DEFAULT_TARGET_KEY = "DISCUTERE_YOUTUBE_API_KEY";

interface Args {
  secret: string;
  version: string;
  project?: string;
  targetKey: string;
}

function usage(exitCode = 1): never {
  console.error(`Usage:
  npm run env:youtube:gcloud -- [--secret NAME] [--project PROJECT_ID] [--version VERSION] [--target-key KEY]

Defaults:
  --secret     ${DEFAULT_GCLOUD_SECRET} (or GCLOUD_YOUTUBE_API_SECRET)
  --version    latest
  --target-key ${DEFAULT_TARGET_KEY}

The secret value is read from Google Secret Manager via gcloud and stored in
Infisical. It is not written to process env or .env.`);
  process.exit(exitCode);
}

function parseArgs(argv: string[]): Args {
  const args: Args = {
    secret: process.env.GCLOUD_YOUTUBE_API_SECRET || DEFAULT_GCLOUD_SECRET,
    version: "latest",
    project: process.env.GCLOUD_PROJECT || process.env.GOOGLE_CLOUD_PROJECT || undefined,
    targetKey: DEFAULT_TARGET_KEY,
  };

  for (let i = 0; i < argv.length; i++) {
    const arg = argv[i];
    const next = () => {
      const value = argv[++i];
      if (!value) usage();
      return value;
    };

    switch (arg) {
      case "--secret":
        args.secret = next();
        break;
      case "--version":
        args.version = next();
        break;
      case "--project":
        args.project = next();
        break;
      case "--target-key":
        args.targetKey = next();
        break;
      case "--help":
      case "-h":
        usage(0);
        break;
      default:
        console.error(`Unknown argument: ${arg}`);
        usage();
    }
  }

  if (!args.secret.trim()) {
    console.error("Error: --secret is required.");
    process.exit(1);
  }
  if (!args.targetKey.trim()) {
    console.error("Error: --target-key is required.");
    process.exit(1);
  }
  return args;
}

function runGcloud(args: Args): Promise<string> {
  const gcloudArgs = [
    "secrets",
    "versions",
    "access",
    args.version,
    "--secret",
    args.secret,
  ];
  if (args.project) gcloudArgs.push("--project", args.project);

  return new Promise((resolve, reject) => {
    const child = spawn("gcloud", gcloudArgs, {
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

function resolveSecretsPath(config: EnvCliConfig): string {
  return config.secretsPath ?? path.join(process.cwd(), ".env.secrets");
}

async function main(): Promise<void> {
  const args = parseArgs(process.argv.slice(2));
  const config = envCliConfig as EnvCliConfig;
  const secretsPath = resolveSecretsPath(config);
  const bootstrap = loadBootstrap(secretsPath, {
    siteUrl: config.defaultSiteUrl,
    environment: config.defaultEnvironment,
  });

  if (!bootstrap) {
    console.error(`Error: Infisical bootstrap is not configured: ${secretsPath}`);
    console.error("Run: npm run env:setup");
    process.exit(1);
  }

  console.log(`Reading Google Secret Manager secret "${args.secret}"...`);
  const value = await runGcloud(args);
  if (!value) {
    console.error("Error: gcloud returned an empty secret value.");
    process.exit(1);
  }

  console.log(`Storing ${args.targetKey} in Infisical (${bootstrap.environment})...`);
  const token = await authenticate(bootstrap);
  await upsertSecret(bootstrap, token, args.targetKey, value);
  console.log(`${args.targetKey} is stored in encrypted config. Use the tuning UI refresh action to reload it without restarting Di.`);
}

main().catch((err) => {
  console.error(`Error: ${err instanceof Error ? err.message : String(err)}`);
  process.exit(1);
});
