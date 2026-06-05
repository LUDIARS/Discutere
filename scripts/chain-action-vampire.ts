/**
 * チェーン議論ドライバ: 「アクションゲームの面白さ」→ その結論(論旨)を踏まえて
 * 「ヴァンサバ(Vampire Survivors)の面白さ」を議論する 2 段構成 (#64 応用)。
 *
 *   node_modules/.bin/tsx scripts/chain-action-vampire.ts --until 07:00 --rounds 14
 *
 * Stage A: アクションゲーム全般の面白さを headless 議論 → 進行役が【収束】で論旨を出す。
 * Stage B: その論旨を前提テキストに注入し、 ヴァンサバの面白さがその論旨にどう当てはまる/
 *          外れるかを headless 議論する。
 * --until 指定で締切まで A→B を繰り返す。 結果は本番 DB (data/) に蓄積。
 */

import Database from "better-sqlite3";

import { createCore } from "../src/core/index.js";
import { PersonasRepo } from "../src/persona-engine/db/personas-repo.js";
import {
  ClaudeCliClient,
  applyPersonaEngineMigrations,
  type LLMClient,
} from "../src/persona-engine/index.js";
import { getConfig } from "../src/config.js";
import { seedHeadlessDiscussion } from "../src/discussion-seed/seed.js";
import { runHeadlessDiscussion } from "../src/discussion-seed/runner.js";

function arg(name: string, def?: string): string | undefined {
  const i = process.argv.indexOf(`--${name}`);
  return i >= 0 ? process.argv[i + 1] : def;
}

/** "HH:MM" → 本日のその時刻の epoch(ms)。 既に過ぎていれば翌日。 */
function deadlineMs(hhmm: string): number {
  const [h, m] = hhmm.split(":").map(Number);
  const d = new Date();
  d.setHours(h, m ?? 0, 0, 0);
  if (d.getTime() <= Date.now()) d.setDate(d.getDate() + 1);
  return d.getTime();
}

const ACTION_TOPIC = {
  title: "【アクションゲーム】何が面白い?",
  description:
    "アクションゲームというジャンルの面白さの核は何か。 操作の手応え・上達・瞬間判断・" +
    "リスクとリワードなど、 プレイヤーが夢中になる構造的な理由を複数の視点から掘り下げる。",
};

function vampireTopic(actionConclusion: string): { title: string; description: string } {
  return {
    title: "ヴァンサバ(Vampire Survivors)の面白さ",
    description:
      "先行議論「アクションゲームの面白さ」は次の論旨に到達した:\n" +
      `「${actionConclusion}」\n\n` +
      "この論旨を前提に、 ヴァンサバ(Vampire Survivors)の面白さを論じる。 " +
      "上の結論がヴァンサバにどう当てはまるか、 あるいは当てはまらない点・ヴァンサバ固有の面白さは何かを、 具体的に掘り下げる。",
  };
}

async function main(): Promise<void> {
  const rounds = Number(arg("rounds", "14"));
  const until = arg("until");
  const label = arg("label", "chain");
  const cfg = getConfig();

  const core = createCore(cfg.discatier.kuzuPath);
  const peDb = new Database(cfg.personaEngine.dbPath);
  applyPersonaEngineMigrations(peDb);
  const personas = new PersonasRepo(peDb);
  const llm: LLMClient = new ClaudeCliClient({
    defaultModel: cfg.llm.model,
    defaultTimeoutMs: cfg.llm.claudeCliTimeoutMs,
    gitBashPath: cfg.llm.gitBashPath,
  });

  const stopAt = until ? deadlineMs(until) : Date.now() + 1; // until 無しなら 1 サイクルだけ
  let cycle = 0;
  do {
    cycle += 1;
    const ts = () => new Date().toTimeString().slice(0, 8);
    console.log(`\n##### [${label}] cycle ${cycle} (${ts()}) — Stage A: アクションゲーム #####`);
    const seedA = seedHeadlessDiscussion({ core, personas, workspaceId: cfg.workspace, topic: ACTION_TOPIC, origin: "chain:action" });
    const a = await runHeadlessDiscussion({
      core, llm, personas, workspaceId: cfg.workspace,
      gapId: seedA.gapId, sessionId: seedA.sessionId,
      options: { maxRounds: rounds, onRound: (r, i) => console.log(`  A round ${r}: utt=${i.utterances}${i.converged ? " [conv]" : ""}`) },
    });
    const conclusion = a.conclusion ?? "(アクションゲームの面白さは多面的で単一の核には収束しなかった)";
    console.log(`  → Stage A conclusion: ${conclusion.slice(0, 120)}`);

    console.log(`##### [${label}] cycle ${cycle} — Stage B: ヴァンサバ (論旨を継承) #####`);
    const topicB = vampireTopic(conclusion);
    const seedB = seedHeadlessDiscussion({ core, personas, workspaceId: cfg.workspace, topic: topicB, origin: "chain:vampire" });
    const b = await runHeadlessDiscussion({
      core, llm, personas, workspaceId: cfg.workspace,
      gapId: seedB.gapId, sessionId: seedB.sessionId,
      options: { maxRounds: rounds, onRound: (r, i) => console.log(`  B round ${r}: utt=${i.utterances}${i.converged ? " [conv]" : ""}`) },
    });
    console.log(`  → Stage B conclusion: ${(b.conclusion ?? "(未収束)").slice(0, 160)}`);
  } while (Date.now() < stopAt);

  console.log(`\n[${label}] done ${new Date().toTimeString().slice(0, 8)} cycles=${cycle}`);
  peDb.close();
  core.close();
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
