/**
 * T2 議論フロー テスト。
 * - FlowDirector.start で rounds × turnsPerRound ターンが回る
 * - ペルソナ選択がランダム
 * - ディスカッションペーパーがロール別に異なる
 * - YouTube 補完条件 (0 件 + 非機密) / 機密/内部では呼ばれない
 * - LLM 失敗時はエラーが発話に出る + llm_call_log に記録される
 */

import assert from "node:assert/strict";
import path from "node:path";
import fs from "node:fs";
import Database from "better-sqlite3";

const TMP_DIR = path.resolve(".tmp/flow-discussion-test");
fs.rmSync(TMP_DIR, { recursive: true, force: true });
fs.mkdirSync(TMP_DIR, { recursive: true });

// テスト用 DB を隔離
const DB_PATH = path.join(TMP_DIR, "test.db");
process.env.DATABASE_PATH = DB_PATH;

// キャッシュをリセットして新規 DB を使う
const { _resetFlowDb } = await import("../../src/flow/db/connection.js");
_resetFlowDb();

const { MockLLMClient } = await import("../../src/persona-engine/llm/mock.js");
const { runDiscussionFlow } = await import("../../src/flow/director.js");
const { buildPersonaPaper, synthesizeOpinions } = await import("../../src/flow/discussion-paper.js");
const { generateFlowPersonas, pickRandomPersona } = await import("../../src/flow/personas.js");
const { investigateTheme } = await import("../../src/flow/investigate.js");
const { canCollectExternal } = await import("../../src/flow/tags.js");
const { _resetConfig } = await import("../../src/config.js");

// ── personas ──────────────────────────────────────────────────────────────────

{
  let seed = 0;
  const rng = () => {
    seed = (seed * 1664525 + 1013904223) & 0xffffffff;
    return (seed >>> 0) / 0x100000000;
  };

  const personas = generateFlowPersonas({ count: 4, defaultModel: "claude-haiku-4-5-20251001", isLocal: false, rng });
  assert.equal(personas.length, 4, "4 personas generated");
  assert.equal(personas[0].role, "facilitator", "first is facilitator");
  const roles = personas.map((p) => p.role);
  assert.ok(roles.includes("opinion"), "has opinion persona");
  assert.ok(roles.includes("debater"), "has debater persona");
  const names = new Set(personas.map((p) => p.name));
  assert.equal(names.size, 4, "all names unique");
  console.log("  [ok] generateFlowPersonas: 4 personas with unique names");
}

{
  const personas = generateFlowPersonas({ count: 1, defaultModel: "m", isLocal: false, rng: () => 0 });
  assert.equal(personas.length, 1, "count=1 returns exactly one persona");
  assert.equal(personas[0].role, "facilitator", "single persona is facilitator");
  console.log("  [ok] generateFlowPersonas: count=1 respected");
}

{
  // ランダム選択の分布確認
  let pick0 = 0, pick1 = 0, pick2 = 0;
  let seed = 12345;
  const rng = () => {
    seed = (seed * 1664525 + 1013904223) & 0xffffffff;
    return (seed >>> 0) / 0x100000000;
  };
  const personas = generateFlowPersonas({ count: 3, defaultModel: "m", isLocal: false, rng });

  let seed2 = 99999;
  const rng2 = () => {
    seed2 = (seed2 * 1664525 + 1013904223) & 0xffffffff;
    return (seed2 >>> 0) / 0x100000000;
  };
  for (let i = 0; i < 300; i++) {
    const picked = pickRandomPersona(personas, rng2);
    if (picked === personas[0]) pick0++;
    else if (picked === personas[1]) pick1++;
    else pick2++;
  }
  // 各ペルソナが少なくとも 10% は選ばれていること (偏りの検証)
  assert.ok(pick0 > 30, `p0 picked ${pick0}/300`);
  assert.ok(pick1 > 30, `p1 picked ${pick1}/300`);
  assert.ok(pick2 > 30, `p2 picked ${pick2}/300`);
  console.log("  [ok] pickRandomPersona: random distribution");
}

// ── discussion-paper ロール別配布 ──────────────────────────────────────────────

{
  const paper = {
    paperId: "p1",
    sessionId: "s1",
    theme: "テストテーマ",
    tags: [],
    mechanics: [{ name: "メカニクスA", description: "説明A", intended_affect: "楽しさ" }],
    supplement: "",
    rounds: [],
  };

  let seed = 42;
  const rng = () => { seed = (seed * 1664525 + 1013904223) & 0xffffffff; return (seed >>> 0) / 0x100000000; };
  const personas = generateFlowPersonas({ count: 4, defaultModel: "m", isLocal: false, rng });

  const debater = personas.find((p) => p.role === "debater")!;
  const opinion = personas.find((p) => p.role === "opinion")!;
  const local = { ...debater, isLocal: true };

  const voices = [{ content: "ユーザの声1", source: "youtube" }];

  // debater (クラウド LLM): userOpinionsText なし
  const dp = buildPersonaPaper({
    paper,
    persona: debater,
    stance: "pro",
    currentRoundUtterances: [],
    userVoices: voices,
    syntheticOpinions: [],
  });
  assert.equal(dp.userOpinionsText, undefined, "debater: no user opinions");

  // opinion: userOpinionsText あり
  const op = buildPersonaPaper({
    paper,
    persona: opinion,
    stance: "opinion",
    currentRoundUtterances: [],
    userVoices: voices,
    syntheticOpinions: [],
  });
  assert.ok(op.userOpinionsText?.includes("ユーザの声1"), "opinion: has user voices");

  // local LLM (debater role): userOpinionsText あり
  const lp = buildPersonaPaper({
    paper,
    persona: local,
    stance: "pro",
    currentRoundUtterances: [],
    userVoices: voices,
    syntheticOpinions: [],
  });
  assert.ok(lp.userOpinionsText?.includes("ユーザの声1"), "local LLM debater: has user voices");

  // 機密: synthetic opinions
  const confidentialPaper = { ...paper, tags: ["機密"] as const };
  const cp = buildPersonaPaper({
    paper: confidentialPaper,
    persona: opinion,
    stance: "opinion",
    currentRoundUtterances: [],
    userVoices: voices,
    syntheticOpinions: ["[想定 (synthetic)] テスト想定意見"],
  });
  assert.ok(cp.userOpinionsText?.includes("synthetic"), "機密: synthetic opinions present");
  assert.ok(!cp.userOpinionsText?.includes("ユーザの声1"), "機密: no real user voices");

  console.log("  [ok] buildPersonaPaper: role-based distribution");
}

// ── investigate: YouTube 補完条件 ─────────────────────────────────────────────

{
  let ytCalled = false;
  const mockYtSearch = async (_q: string, _max: number) => {
    ytCalled = true;
    return ["コメント1", "コメント2"];
  };

  // 非機密 + 0 件 → YouTube 呼ばれる
  ytCalled = false;
  const r1 = await investigateTheme({
    theme: "テストテーマ",
    tags: ["開発"],
    getSentimentCount: () => 0,
    youtubeSearch: mockYtSearch,
    gamesDir: path.join(TMP_DIR, "no-games-dir"),
  });
  assert.ok(ytCalled, "YouTube called when 0 sentiment + non-confidential");
  assert.ok(r1.youtubeUsed, "youtubeUsed=true");
  assert.equal(r1.youtubeComments.length, 2, "2 comments returned");

  // 機密 → YouTube 呼ばれない
  ytCalled = false;
  await investigateTheme({
    theme: "テストテーマ",
    tags: ["機密"],
    getSentimentCount: () => 0,
    youtubeSearch: mockYtSearch,
    gamesDir: path.join(TMP_DIR, "no-games-dir"),
  });
  assert.ok(!ytCalled, "YouTube NOT called for 機密");

  // 内部 → YouTube 呼ばれない
  ytCalled = false;
  await investigateTheme({
    theme: "テストテーマ",
    tags: ["内部"],
    getSentimentCount: () => 0,
    youtubeSearch: mockYtSearch,
    gamesDir: path.join(TMP_DIR, "no-games-dir"),
  });
  assert.ok(!ytCalled, "YouTube NOT called for 内部");

  // 感情データあり → YouTube 呼ばれない
  ytCalled = false;
  await investigateTheme({
    theme: "テストテーマ",
    tags: [],
    getSentimentCount: () => 5,
    youtubeSearch: mockYtSearch,
    gamesDir: path.join(TMP_DIR, "no-games-dir"),
  });
  assert.ok(!ytCalled, "YouTube NOT called when sentiment count > 0");

  console.log("  [ok] investigateTheme: YouTube 補完条件");
}

{
  // YouTube 取得前に警告が出る
  const warnings: string[] = [];
  await investigateTheme({
    theme: "ジャーニー",
    tags: [],
    getSentimentCount: () => 0,
    youtubeSearch: async () => [],
    warn: (m) => warnings.push(m),
    gamesDir: path.join(TMP_DIR, "no-games-dir"),
  });
  assert.ok(warnings.some((w) => w.includes("YouTube")), "warning issued before YouTube fetch");
  console.log("  [ok] investigateTheme: warns before YouTube fetch");
}

// ── FlowDirector 統合 ──────────────────────────────────────────────────────────

{
  // rounds × turnsPerRound のターンが回り発話が永続される
  _resetFlowDb();
  process.env.DATABASE_PATH = path.join(TMP_DIR, "integration.db");

  // 小さな config にするため env をセット
  process.env.DISCUTERE_FLOW_ROUNDS = "2";
  process.env.DISCUTERE_FLOW_TURNS_PER_ROUND = "3";
  process.env.DISCUTERE_FLOW_PERSONA_COUNT = "3";

  const callLog: string[] = [];
  // 常に空でない応答を返す mock
  const mock = new MockLLMClient(
    Array(100).fill(() => "テスト発話"),
    "フォールバック発話"
  );

  let seed = 1234;
  const rng = () => { seed = (seed * 1664525 + 1013904223) & 0xffffffff; return (seed >>> 0) / 0x100000000; };

  const result = await runDiscussionFlow("ジャーニーの感情設計", [], {
    llm: mock,
    rng,
    gamesDir: path.join(TMP_DIR, "no-games-dir"),
  });

  // rounds × turnsPerRound = 2 × 3 = 6 ターン分の発話が永続されるはず
  const db = new Database(process.env.DATABASE_PATH);
  const rows = db
    .prepare("SELECT * FROM flow_utterance WHERE session_id = ?")
    .all(result.sessionId) as Array<Record<string, unknown>>;
  db.close();

  assert.ok(rows.length > 0, `utterances persisted (got ${rows.length})`);
  // 2 rounds × (3 regular turns + 1 facilitator opening) = 8 max
  assert.ok(rows.length <= 8, `max 8 utterances (got ${rows.length})`);
  assert.equal(result.rounds, 2, "2 rounds completed");
  console.log(`  [ok] runDiscussionFlow: ${rows.length} utterances persisted over 2 rounds`);

  // discussion_paper が DB にある
  const db2 = new Database(process.env.DATABASE_PATH);
  const paperRow = db2
    .prepare("SELECT * FROM discussion_paper WHERE session_id = ?")
    .get(result.sessionId) as Record<string, unknown> | undefined;
  db2.close();
  assert.ok(paperRow, "discussion_paper created");
  assert.equal(paperRow!.theme, "ジャーニーの感情設計", "theme stored");
  console.log("  [ok] discussion_paper persisted");

  // クリーンアップ
  delete process.env.DISCUTERE_FLOW_ROUNDS;
  delete process.env.DISCUTERE_FLOW_TURNS_PER_ROUND;
  delete process.env.DISCUTERE_FLOW_PERSONA_COUNT;
}

{
  // LLM 失敗時: エラーが発話に出て llm_call_log に記録される
  _resetFlowDb();
  _resetConfig();
  process.env.DATABASE_PATH = path.join(TMP_DIR, "error-test.db");
  process.env.DISCUTERE_FLOW_ROUNDS = "1";
  process.env.DISCUTERE_FLOW_TURNS_PER_ROUND = "1";
  process.env.DISCUTERE_FLOW_PERSONA_COUNT = "2";

  const errorMock = new MockLLMClient([
    () => "[]",                                        // ペルソナ価値軸生成 (respec 01・空割当)
    () => ({ ok: false as const, error: "API timeout" }), // ファシリテーター開幕がエラーになる
  ]);

  let seed = 777;
  const rng = () => { seed = (seed * 1664525 + 1013904223) & 0xffffffff; return (seed >>> 0) / 0x100000000; };

  const result = await runDiscussionFlow("エラーテスト", [], {
    llm: errorMock,
    rng,
    gamesDir: path.join(TMP_DIR, "no-games-dir"),
  });

  const db = new Database(process.env.DATABASE_PATH);
  const errRows = db
    .prepare("SELECT * FROM flow_utterance WHERE session_id = ? AND is_error = 1")
    .all(result.sessionId) as Array<Record<string, unknown>>;
  const costRows = db
    .prepare("SELECT * FROM llm_call_log WHERE session_id = ?")
    .all(result.sessionId) as Array<Record<string, unknown>>;
  db.close();

  assert.ok(errRows.length > 0, `error utterance recorded (got ${errRows.length})`);
  assert.ok(errRows[0].text.toString().includes("エラー"), "error text in utterance");
  assert.ok(costRows.length > 0, `llm_call_log entries (got ${costRows.length})`);
  console.log("  [ok] LLM error: utterance is_error=1, llm_call_log recorded");

  delete process.env.DISCUTERE_FLOW_ROUNDS;
  delete process.env.DISCUTERE_FLOW_TURNS_PER_ROUND;
  delete process.env.DISCUTERE_FLOW_PERSONA_COUNT;
}

// ── loadMechanics: mechanics が frontmatter の最後のキーでも全件取れる ──────────

{
  // 最後キー再現ファイル: mechanics の後にキーが無い
  const testGamesDir = path.join(TMP_DIR, "games-last-key");
  fs.mkdirSync(testGamesDir, { recursive: true });
  fs.writeFileSync(
    path.join(testGamesDir, "test-game.md"),
    [
      "---",
      "id: test-game",
      "title: テストゲーム",
      "mechanics:",
      "  - name: 飛行",
      "    description: 空を飛ぶ",
      "    intended_affect: 高揚",
      "    intended_valence: positive",
      "  - name: 収集",
      "    description: アイテムを集める",
      "    intended_affect: 達成感",
      "    intended_valence: positive",
      "---",
      "",
      "# 概要",
      "テスト用のゲームデータ。",
    ].join("\n")
  );

  const result = await investigateTheme({
    theme: "飛行",
    tags: [],
    gamesDir: testGamesDir,
    getSentimentCount: () => 0,
  });

  // mechanics が最後のキーでも全件ロードされる
  assert.ok(result.mechanics.length >= 1, `mechanics loaded (got ${result.mechanics.length})`);
  const names = result.mechanics.map((m) => m.name);
  assert.ok(names.includes("飛行"), `mechanics includes "飛行" (got ${JSON.stringify(names)})`);
  assert.ok(names.includes("収集"), `mechanics includes "収集" (got ${JSON.stringify(names)})`);
  console.log("  [ok] loadMechanics: mechanics as last frontmatter key → all items loaded");
}

{
  const testGamesDir = path.join(TMP_DIR, "games-no-cross-contam");
  fs.mkdirSync(testGamesDir, { recursive: true });
  fs.writeFileSync(
    path.join(testGamesDir, "death-stranding.md"),
    [
      "---",
      "id: death-stranding",
      'title: "DEATH STRANDING"',
      "mechanics:",
      "  - name: 積荷のバランス管理",
      "    description: 総重量と積み付けがサムの歩行安定に影響する",
      "    intended_affect: 重荷感",
      "---",
    ].join("\n")
  );
  fs.writeFileSync(
    path.join(testGamesDir, "journey.md"),
    [
      "---",
      "id: journey",
      'title: "Journey (風ノ旅ビト)"',
      "mechanics:",
      "  - name: スカーフと飛行",
      "    description: スカーフが浮遊の燃料になる",
      "    intended_affect: 高揚",
      "---",
    ].join("\n")
  );

  const unrelated = await investigateTheme({
    theme: "ローグライトの報酬設計",
    tags: [],
    gamesDir: testGamesDir,
    getSentimentCount: () => 0,
  });
  assert.deepEqual(unrelated.mechanics, [], "無関係なゲームの先頭メカニクスを混入させない");

  const journey = await investigateTheme({
    theme: "Journey の飛行",
    tags: [],
    gamesDir: testGamesDir,
    getSentimentCount: () => 0,
  });
  const names = journey.mechanics.map((m) => m.name);
  assert.ok(names.includes("スカーフと飛行"), `Journey mechanics loaded (${JSON.stringify(names)})`);
  assert.ok(!names.includes("積荷のバランス管理"), "Death Stranding mechanics are not mixed into Journey");
  console.log("  [ok] loadMechanics: unrelated game mechanics are not mixed into papers");
}

// クリーンアップ
delete process.env.DATABASE_PATH;
_resetFlowDb();
