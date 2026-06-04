/**
 * Headless 議論ランナー CLI (#64)。
 *
 *   npm run headless                              # mock LLM / tmp DB / ジャンル種で 1 回
 *   npm run headless -- --runs 3                  # 3 回まわす (種を順に変える)
 *   npm run headless -- --genre アクション         # ジャンル指定
 *   npm run headless -- --topic "ローグライトの中毒性とは"   # 任意議題
 *   npm run headless -- --llm cli                 # 実 LLM (claude CLI 経由)
 *   npm run headless -- --llm anthropic           # 実 LLM (ANTHROPIC_API_KEY)
 *   npm run headless -- --persist                 # 本番 DB (data/) に書く (既定は .tmp)
 *
 * 進行役 (司会 結) が開幕で登場 → 視点違いの persona が議論 → 止揚が溜まったら
 * 進行役が【収束】で締める、 までを 1 プロセスで完結させる。
 */

import fs from "node:fs";
import path from "node:path";
import Database from "better-sqlite3";

import { createCore } from "../src/core/index.js";
import { PersonasRepo } from "../src/persona-engine/db/personas-repo.js";
import {
  AnthropicSdkClient,
  ClaudeCliClient,
  applyPersonaEngineMigrations,
  type LLMClient,
  type LLMInvokeArgs,
} from "../src/persona-engine/index.js";
import { getConfig } from "../src/config.js";
import { GENRE_SEEDS, buildGenreTopic, pickGenreSeed, type SeedTopic } from "../src/discussion-seed/genres.js";
import { seedHeadlessDiscussion } from "../src/discussion-seed/seed.js";
import { runHeadlessDiscussion } from "../src/discussion-seed/runner.js";

interface Args {
  runs: number;
  rounds: number;
  llm: "mock" | "cli" | "anthropic";
  topic?: string;
  genre?: string;
  index: number;
  persist: boolean;
}

function parseArgs(argv: string[]): Args {
  const get = (name: string): string | undefined => {
    const i = argv.indexOf(`--${name}`);
    return i >= 0 ? argv[i + 1] : undefined;
  };
  const llmRaw = get("llm") ?? "mock";
  return {
    runs: Number(get("runs") ?? 1),
    rounds: Number(get("rounds") ?? 40),
    llm: llmRaw === "cli" || llmRaw === "anthropic" ? llmRaw : "mock",
    topic: get("topic"),
    genre: get("genre"),
    index: Number(get("index") ?? 0),
    persist: argv.includes("--persist"),
  };
}

/** prompt 種別を見て妥当な JSON を返すオフライン mock (機構検証用)。 */
function createScriptedMock(): LLMClient {
  let expandCount = 0;
  let judgeCount = 0;
  const personaPool = [
    { name: "体験設計派", display_name: "設計 創", trait: "構造", opening: "面白さは『上達の手応え』が連続することにある。報酬より成長曲線が核だと思う。" },
    { name: "感情重視派", display_name: "情野 響", trait: "情緒", opening: "いや、夢中になるのは感情の揺れだ。緊張と緩和のリズムこそが中毒性を生む。" },
    { name: "懐疑派", display_name: "疑田 慎", trait: "批判", opening: "上達も感情も結局は『分かりやすい目標』があってこそ。目標設計が無いと両方空回りする。" },
    { name: "社会性派", display_name: "繋木 縁", trait: "共有", opening: "一人の体験だけでは続かない。他者と共有・競争できる構造が長期の熱を支える。" },
  ];
  const responder = (args: LLMInvokeArgs): string => {
    const p = args.prompt;
    if (p.includes('"opening"')) {
      const persona = personaPool[expandCount % personaPool.length];
      expandCount += 1;
      return JSON.stringify({
        name: persona.name,
        display_name: persona.display_name,
        description: `${persona.name}の立場から論点を出す参加者。`,
        traits: [persona.trait],
        speech_style: "率直で具体的。",
        opening: persona.opening,
      });
    }
    if (p.includes('"found"')) {
      judgeCount += 1;
      // 数発言たまってから止揚を検出する (毎回ユニークな summary)。
      const found = judgeCount >= 2;
      return JSON.stringify({
        found,
        summary: found
          ? `止揚${judgeCount}: 成長実感・感情の起伏・明確な目標・他者との共有が噛み合った時に「面白さ」が立ち上がる、という統合的理解。`
          : "",
      });
    }
    // converge
    return JSON.stringify({
      summary:
        "面白さの核は単一要素ではなく、『上達の手応え』『感情の起伏』『明確な目標』『他者との共有』が相互に支え合う構造にある。どれか一つが欠けると熱は続かない。",
    });
  };
  // MockLLMClient は responders を順番に消費する設計で prompt 種別を見分けられないため、
  // ここでは prompt を inspect して JSON 種別を切り替える軽量 client を返す。
  return {
    async invoke(args: LLMInvokeArgs): Promise<{ ok: true; text: string }> {
      return { ok: true, text: responder(args) };
    },
  };
}

function buildLlm(kind: Args["llm"]): LLMClient {
  if (kind === "cli") {
    const cfg = getConfig();
    return new ClaudeCliClient({
      defaultModel: cfg.llm.model,
      defaultTimeoutMs: cfg.llm.claudeCliTimeoutMs,
      gitBashPath: cfg.llm.gitBashPath,
    });
  }
  if (kind === "anthropic") {
    const cfg = getConfig();
    return new AnthropicSdkClient({ apiKey: cfg.llm.anthropicApiKey, defaultModel: cfg.llm.model });
  }
  return createScriptedMock();
}

function resolveTopic(args: Args, runIndex: number): SeedTopic {
  if (args.topic) return { title: args.topic, description: args.topic };
  if (args.genre) {
    const seed = GENRE_SEEDS.find((g) => g.genre === args.genre) ?? { genre: args.genre };
    return buildGenreTopic(seed);
  }
  return buildGenreTopic(pickGenreSeed(args.index + runIndex));
}

async function main(): Promise<void> {
  const args = parseArgs(process.argv.slice(2));
  const workspaceId = getConfig().workspace;

  let core: ReturnType<typeof createCore>;
  let peDb: Database.Database;
  if (args.persist) {
    const cfg = getConfig();
    core = createCore(cfg.discatier.kuzuPath);
    peDb = new Database(cfg.personaEngine.dbPath);
  } else {
    const workDir = path.resolve(".tmp/headless-discuss");
    fs.rmSync(workDir, { recursive: true, force: true });
    fs.mkdirSync(workDir, { recursive: true });
    core = createCore(path.join(workDir, "discatier.kuzu"), path.join(workDir, "events.jsonl"));
    peDb = new Database(path.join(workDir, "persona-engine.db"));
  }
  applyPersonaEngineMigrations(peDb);
  const personas = new PersonasRepo(peDb);
  // mock は議論ごとに状態 (登場 persona / 止揚カウンタ) をリセットしたいので run 毎に作る。
  // 実 LLM (cli/anthropic) は state を持たないので使い回す。
  const sharedLlm = args.llm === "mock" ? null : buildLlm(args.llm);

  console.log(`# headless-discuss (llm=${args.llm}, runs=${args.runs}, persist=${args.persist})\n`);

  for (let r = 0; r < args.runs; r++) {
    const topic = resolveTopic(args, r);
    console.log(`\n=== run ${r + 1}/${args.runs}: ${topic.title} ===`);
    const seeded = seedHeadlessDiscussion({ core, personas, workspaceId, topic, origin: "auto:genre" });
    const result = await runHeadlessDiscussion({
      core,
      llm: sharedLlm ?? buildLlm("mock"),
      personas,
      workspaceId,
      gapId: seeded.gapId,
      sessionId: seeded.sessionId,
      options: {
        maxRounds: args.rounds,
        onRound: (round, info) =>
          console.log(`  round ${round}: utterances=${info.utterances}${info.converged ? " [converged]" : ""}`),
      },
    });
    printTranscript(core, seeded.sessionId);
    console.log(
      `  → converged=${result.converged} rounds=${result.rounds} utterances=${result.utteranceCount}`
    );
  }

  peDb.close();
  core.close();
}

function printTranscript(core: ReturnType<typeof createCore>, sessionId: string): void {
  const rows = core.client.raw
    .prepare("SELECT speaker_id, raw_content FROM utterances WHERE session_id = ? ORDER BY posted_at ASC")
    .all(sessionId) as Array<{ speaker_id: string | null; raw_content: string }>;
  console.log("  --- transcript ---");
  for (const u of rows) {
    const who = (u.speaker_id ?? "?").replace(/^persona:/, "");
    console.log(`  [${who}] ${u.raw_content.replace(/\n/g, " ")}`);
  }
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
