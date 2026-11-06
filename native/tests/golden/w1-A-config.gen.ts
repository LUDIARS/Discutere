// src/config.ts (Node 実装) の loadConfig の入出力を記録する。
// 実行: npx tsx native/tests/golden/w1-A-config.gen.ts
// 出力: native/tests/fixtures/config/load.json ({ name, env, file, ok, config | error }[])
// env はプロセスの環境を丸ごと置き換えた合成値、file は config ファイルの生テキスト (null = 無し)。
// 出力中の作業ディレクトリは "<CWD>" に置き換える。秘密値はすべて明らかな偽物。
import fs from "node:fs";
import os from "node:os";
import path from "node:path";

type Case = { name: string; env: Record<string, string>; file: string | null };
const CFG = "./discutere.golden.json";
const K = "\u212A"; // Kelvin sign (JS の toLowerCase で k になる)

const fullFile = {
  server: { port: "4010", host: "0.0.0.0", frontendUrl: "" },
  flow: {
    rounds: 5, turnsPerRound: 2.5, personaCount: "7", voterCount: true,
    roster: { facilitator: "claude-x@high", discussants: [" a@low ", "", 3, "b@high"] },
    voteLenses: [{ key: "k1", instruction: "i1" }, { key: " ", instruction: "x" }, "bad", { key: "k2", instruction: "i2" }],
    convergeShare: 0.75, engine: "rounds",
    dialectic: { rebutTurns: 9, judgeModel: "judge-x", ruleCheck: false, finalJudgeModel: "final-x" },
    tags: ["機密", " 開発 "],
    autoCrawl: { enabled: false, source: "youtube" },
    informationGating: { minDensity: "rich", maxLearnIterations: "3" },
    paperReview: { enabled: true, timeoutMs: 1000 },
    userVoices: { steamLanguages: "japanese", voluptasBaseUrl: "https://voluptas.example" },
    foresight: { turns: 99.7 },
    proxy: { maxBudgetTokens: 100, budgetTokens: 50000 },
    anatomia: { enabled: true, binPath: "/opt/anatomia.mjs" },
  },
  workspace: "ws-file",
  discatier: {
    kuzuPath: "/kg/default.db",
    knowledgeGraphs: [{ id: "a", label: "A", kuzuPath: "/kg/a.db" }, { id: 1, kuzuPath: "/x" }, null, { id: "b", kuzuPath: "/kg/b.db" }],
  },
  personaEngine: { enabled: true, tickMs: "250" },
  autoSeed: { sources: [] },
  llm: { backend: "Claude-CLI", gitBashPath: "C:/git/bash.exe", tiers: { cheap: "cheap-x" }, local: { apiKey: "fake-local-key" } },
  embedding: { timeoutMs: "90000", batchSize: 8 },
  classifier: { backend: "OFF", model: "cls-x" },
  discussion: { opinionModels: [{ model: "m1", weight: 3 }], keymanModel: "keyman-x" },
  workerPool: { gitBashPath: null, workers: [{ id: "w1", role: "pro", provider: "claude", model: "claude-a" }], excludeProviders: ["codex", " "] },
  discord: {
    botToken: "fake-discord-token", guildIds: ["g1", "g2"], guildId: "g1", adminIds: ["u1"],
    forum: { enabled: false, improvementTagNames: ["改善X"] }, gameFeedback: { categoryName: "感想" },
  },
  slack: { enabled: true, appToken: "xapp-fake", channelIds: ["C1"] },
  backup: { bucket: "bucket-file", region: "us-west-2", endpoint: "http://minio.local:9000", intervalDays: "7" },
  fundamentum: { enabled: false, dataDir: "fm-data" },
  kgSync: { sourceUrl: "https://kg.example/sync", auto: true, intervalHours: 12 },
  cost: { relay: { enabled: true, concordiaUrl: "http://localhost:17330" } },
};

const cases: Case[] = [
  { name: "defaults", env: { DISCUTERE_CONFIG: CFG }, file: null },
  {
    name: "env-overrides",
    env: {
      DISCUTERE_CONFIG: CFG,
      NODE_ENV: "production",
      BACKEND_PORT: " 0x10 ",
      BACKEND_HOST: "",
      DISCUTERE_FLOW_ROUNDS: "1e1",
      DISCUTERE_FLOW_TURNS_PER_ROUND: "abc",
      DISCUTERE_FLOW_PERSONA_COUNT: "Infinity",
      DISCUTERE_FLOW_TAGS: "機密, 開発,, ",
      DISCUTERE_FLOW_ROSTER_DISCUSSANTS: "claude-a@high,gpt-b@low",
      DISCUTERE_FLOW_VOTE_LENSES: JSON.stringify([{ key: "cost", instruction: "コスト効率を最重視して選ぶ" }]),
      DISCUTERE_FLOW_ENGINE: "Dialectic",
      DISCUTERE_FLOW_DIALECTIC_RULE_CHECK: " YES ",
      DISCUTERE_FLOW_PAPER_REVIEW_ENABLED: "On",
      DISCUTERE_FLOW_AUTOCRAWL_SOURCE: "youtube",
      DISCUTERE_FLOW_FORESIGHT_TURNS: "-4",
      DISCUTERE_FLOW_PROXY_MAX_BUDGET_TOKENS: "4000.9",
      DISCUTERE_FLOW_PROXY_BUDGET_TOKENS: "9999",
      LLM_BACKEND: `WOR${K}ER-POOL`,
      ANTHROPIC_API_KEY: "sk-fake-not-a-key",
      LLM_TIER_STRONG: "strong-x",
      DISCUTERE_EMBEDDING_TIMEOUT_MS: " 5.0 ",
      VOLPUTAS_URL: "http://127.0.0.1:4500",
      DISCUTERE_BACKUP_REGION: "",
      AWS_REGION: "eu-west-1",
      AWS_ACCESS_KEY_ID: "AKIAFAKEFAKEFAKE",
      AWS_SECRET_ACCESS_KEY: "fake-secret",
      DISCUTERE_DISCORD_GUILD_IDS: "g1,g2,g1",
      DISCUTERE_DISCORD_GUILD_ID: "g3",
      DISCUTERE_DISCORD_ADMIN_IDS: " , ",
      DISCUTERE_WORKER_POOL_EXCLUDE_PROVIDERS: " , ",
      CLAUDE_CODE_GIT_BASH_PATH: "/usr/bin/bash",
      DISCUTERE_SLACK_BOT_TOKEN: "xoxb-fake",
      DISCUTERE_EXTERNAL_DISCUSSION_SECRET: "",
      DISCUTERE_GLAB_CONCLUSIONS_TOKEN: "fake-glab-token",
      LUDIARS_ONESHOT_MODEL_OPUS: "claude-opus-override",
      DISCUTERE_KG_SYNC_AUTO: "0",
    },
    file: null,
  },
  { name: "file", env: { DISCUTERE_CONFIG: CFG }, file: JSON.stringify(fullFile, null, 2) },
  {
    name: "env-beats-file",
    env: {
      DISCUTERE_CONFIG: CFG,
      BACKEND_PORT: "5000",
      DISCUTERE_FLOW_TAGS: "運用",
      DISCUTERE_BACKUP_REGION: "",
      LLM_BACKEND: "mock",
      DISCUTERE_CLASSIFIER_BACKEND: "nope",
      DISCUTERE_DISCORD_FORUM_ENABLED: "false",
      DISCUTERE_FLOW_VOTE_LENSES: "{broken",
      DISCUTERE_WORKER_POOL_GIT_BASH: "",
      DISCUTERE_FLOW_USER_VOICES_STEAM_LANGUAGES: "english",
      DISCUTERE_FUNDAMENTUM_DATA_DIR: "/abs/fm",
    },
    file: JSON.stringify(fullFile),
  },
  { name: "broken-file", env: { DISCUTERE_CONFIG: CFG }, file: "{ \"server\": { \"port\": 1 " },
  { name: "array-file", env: { DISCUTERE_CONFIG: CFG }, file: "[1,2,3]" },
  { name: "null-file", env: { DISCUTERE_CONFIG: CFG }, file: "null" },
  { name: "string-sections", env: { DISCUTERE_CONFIG: CFG }, file: JSON.stringify({ flow: "x", server: [1], llm: null, discord: { forum: 3 } }) },
  { name: "batch-size-zero", env: { DISCUTERE_CONFIG: CFG, DISCUTERE_EMBEDDING_BATCH_SIZE: "0" }, file: null },
  { name: "timeout-not-integer", env: { DISCUTERE_CONFIG: CFG }, file: JSON.stringify({ embedding: { timeoutMs: 2.5 } }) },
  { name: "timeout-bool", env: { DISCUTERE_CONFIG: CFG }, file: JSON.stringify({ embedding: { timeoutMs: true } }) },
  { name: "timeout-empty-string", env: { DISCUTERE_CONFIG: CFG }, file: JSON.stringify({ embedding: { batchSize: "" } }) },
  { name: "default-config-path", env: {}, file: null },
];

const out: unknown[] = [];
const origCwd = process.cwd();
const savedEnv = { ...process.env };
const { loadConfig } = await import("../../../src/config.js");
for (const c of cases) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), "di-golden-config-"));
  const real = fs.realpathSync(dir);
  if (c.file !== null) fs.writeFileSync(path.join(real, "discutere.golden.json"), c.file);
  for (const k of Object.keys(process.env)) delete process.env[k];
  Object.assign(process.env, c.env);
  process.chdir(real);
  try {
    const cfg = loadConfig();
    const text = JSON.stringify(cfg).split(real).join("<CWD>");
    out.push({ name: c.name, env: c.env, file: c.file, ok: true, config: JSON.parse(text) });
  } catch (e) {
    out.push({ name: c.name, env: c.env, file: c.file, ok: false, error: (e as Error).message });
  } finally {
    process.chdir(origCwd);
    fs.rmSync(real, { recursive: true, force: true });
  }
}
for (const k of Object.keys(process.env)) delete process.env[k];
Object.assign(process.env, savedEnv);
const dirOut = path.join(path.dirname(new URL(import.meta.url).pathname), "..", "fixtures", "config");
fs.mkdirSync(dirOut, { recursive: true });
fs.writeFileSync(path.join(dirOut, "load.json"), JSON.stringify(out, null, 2) + "\n");
console.log(`wrote ${out.length} cases`);
