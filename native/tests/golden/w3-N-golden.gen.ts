// w3-N (外部の声の検索・Discatier アダプタ・md 書き出し・投稿分類・コマンド処理) の Node 実装の入出力を記録する。
// 実行: npx tsx native/tests/golden/w3-N-golden.gen.ts
// 出力: native/tests/fixtures/w3n/*.json (値はすべて架空。DB は一時ディレクトリの合成データ)
//
// DB を使うものは「Node で組んだ DB の表 (rowid 順) をそのまま記録 → C++ は同じ行を入れて同じ操作をする」形。
import fs from "node:fs";
import os from "node:os";
import path from "node:path";

const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "w3n-golden-"));
// config は遅延ロード。実在の設定ファイルを読まず、埋め込みは有効 (接続先は閉じたポート。温めは事前に済ませる)。
process.env.DISCUTERE_CONFIG = path.join(tmp, "no-config.json");
process.env.DISCATIER_KUZU_PATH = path.join(tmp, "kg", "discatier.kuzu");
process.env.DISCUTERE_EMBEDDING_ENABLED = "1";
process.env.DISCUTERE_EMBEDDING_MODEL = "golden-model";
process.env.DISCUTERE_EMBEDDING_BASE_URL = "http://127.0.0.1:9/v1";

const { createCore } = await import("../../../src/core/index.js");
const { _resetConfig } = await import("../../../src/config.js");
const { normalizeQueryText, warmQueryEmbedding, _resetQueryEmbedCache } = await import(
  "../../../src/core/vectors/query-embed-cache.js"
);
const { excludeUtterance } = await import("../../../src/core/noise/exclusions.js");
const { ensureReactionTables } = await import("../../../src/discord-hook/reactions.js");
const { searchExternalVoiceRows } = await import("../../../src/discatier-engine-adapter/voice-search.js");
const { createDiscatierContextProvider } = await import("../../../src/discatier-engine-adapter/index.js");
const { openAttributionStore } = await import("../../../src/crawler/sources/attribution-store.js");
const md = await import("../../../src/visualize/md-exporter.js");
const ad = await import("../../../src/discord-hook/auto-discussion.js");
const { MockLLMClient } = await import("../../../src/persona-engine/index.js");
const cross = await import("../../../src/core/projection/command-handlers/cross/index.js");
const emotion = await import("../../../src/core/projection/command-handlers/emotion/index.js");
const learning = await import("../../../src/core/projection/command-handlers/learning/index.js");
const synthesis = await import("../../../src/core/projection/command-handlers/synthesis/index.js");

_resetConfig();
_resetQueryEmbedCache();

const root = path.join(path.dirname(new URL(import.meta.url).pathname), "..", "fixtures", "w3n");
fs.mkdirSync(root, { recursive: true });
const write = (name: string, value: unknown) => {
  fs.writeFileSync(path.join(root, name), JSON.stringify(value, null, 2) + "\n");
  console.log(`wrote ${name}`);
};
const run = async <T>(fn: () => T | Promise<T>) => {
  try {
    return { ok: true, value: JSON.parse(JSON.stringify(await fn())) };
  } catch (e) {
    return { ok: false, error: (e as Error).message };
  }
};

type Core = ReturnType<typeof createCore>;
const TABLES = [
  "people",
  "games",
  "mechanics",
  "aesthetics",
  "affects",
  "sessions",
  "utterances",
  "design_gaps",
  "hypotheses",
  "hypothesis_validations",
  "embeddings",
  "opinion_scores",
  "utterance_exclusions",
  "embedding_query_cache",
];
function dumpTables(core: Core): Record<string, unknown[]> {
  const out: Record<string, unknown[]> = {};
  for (const t of TABLES) {
    const exists = core.client.raw
      .prepare("SELECT name FROM sqlite_master WHERE type = 'table' AND name = ?")
      .get(t);
    if (!exists) continue;
    out[t] = core.client.raw.prepare(`SELECT * FROM ${t} ORDER BY rowid`).all();
  }
  return out;
}

const T0 = 1_700_000_000_000;
const ws = "knowledge";

// ───────────────────────── voice search ─────────────────────────
{
  const core = createCore(process.env.DISCATIER_KUZU_PATH, path.join(tmp, "events.jsonl"));
  ensureReactionTables(core.client.raw);
  core.repos.game.create({ id: "g-star", workspaceId: ws, title: "Star Quest (スタークエスト)" });
  core.repos.game.create({ id: "g-dragon", workspaceId: ws, title: "Dragon Saga" });
  const longText =
    "スタークエストのボス戦について長文で語る🎮。" + "序盤から終盤まで敵の配置と報酬の釣り合いが気になる。".repeat(12);
  const utts: Array<[string, string | null, string]> = [
    ["v01", "ext:youtube:UC1", "スタークエストのボス戦は理不尽すぎる"],
    ["v02", "ext:steam:7656", "Star Quest boss fight is UNFAIR"],
    ["v03", "ext:youtube:UC2", "スタークエストのボス戦は理不尽すぎる！"],
    ["v04", "ext:niconico:n1", "ボス戦の難易度が高い 100%_理不尽"],
    ["v05", "persona:advocate", "ボス戦は歯ごたえがあって良い"],
    ["v06", "ext:niconico:n2", "今日は良い天気ですね"],
    ["v07", "ext:niconico:n3", "ボス戦 最高"],
    ["v08", "ext:test:a", "経験値とXPの両方が成長実感につながる"],
    ["v09", "ext:test:b", "経験値だけでは単調に感じる"],
    ["v10", "ext:test:c", "XPの表示方法を変えてほしい"],
    ["v11", "ext:test:d", "xp が足りない"],
    ["v12", "ext:youtube:UC3", longText],
    ["v13", "ext:steam:9999", "ドラゴンサーガ? Dragon Saga の戦闘はテンポが良い"],
    ["v14", "ext:youtube:UC4", "ボス戦BGMが好き"],
    ["v15", "ext:youtube:UC5", "ボス戦BGMが好き。"],
  ];
  for (let i = 0; i < 20; i++) utts.push([`f${String(i).padStart(2, "0")}`, "ext:niconico:filler", `無関係な雑談コメント番号 ${i}`]);
  utts.forEach(([id, speaker, text], i) => {
    core.repos.utterance.create({
      id,
      workspaceId: ws,
      sessionId: "ext:session",
      speakerId: speaker ?? undefined,
      rawContent: text,
      postedAt: T0 + i * 1000,
    });
  });
  const opinion = core.client.raw.prepare(
    "INSERT INTO opinion_scores (target_id, target_kind, score, updated_at) VALUES (?, 'utterance', ?, ?)"
  );
  opinion.run("v06", 3, T0);
  opinion.run("v09", 5, T0);
  opinion.run("v02", -2, T0);
  opinion.run("v14", 1, T0);
  excludeUtterance(core.client.raw, "v07", "noise");
  const vec = (id: string, v: number[]) =>
    core.vectors.registerEmbedding({ workspaceId: ws, nodeType: "utterance", nodeId: id, vector: v });
  vec("v08", [0, 1]);
  vec("v09", [1, 0]);
  vec("v10", [0.9, 0.1]);
  vec("v01", [0.2, 0.8]);
  vec("v03", [0.21, 0.79]);
  vec("v04", [0.7, 0.3]);
  vec("v12", [0.5, 0.5]);
  vec("v14", [0.6, 0.4]);
  vec("v15", [0.6, 0.41]);
  const warmed: Array<{ terms: string[]; vector: number[] }> = [
    { terms: ["経験値"], vector: [1, 0] },
    { terms: ["XP"], vector: [0.9, 0.1] },
    { terms: ["ボス戦"], vector: [0.65, 0.35] },
    { terms: ["スタークエストのボス戦"], vector: [0.1, 0.9] },
    { terms: ["100%"], vector: [0.7, 0.31] },
  ];
  for (const w of warmed) {
    await warmQueryEmbedding(
      core.client.raw,
      { embed: async (texts: string[]) => texts.map(() => w.vector) },
      "golden-model",
      normalizeQueryText(w.terms),
      () => undefined
    );
  }

  const attribution = openAttributionStore();
  attribution.set({ utteranceId: "v01", source: "youtube", sourceUrl: "https://www.youtube.com/watch?v=vid1&lc=c1", nativeId: "c1", gameSlug: "star-quest", at: T0 });
  attribution.set({ utteranceId: "v12", source: "youtube", sourceUrl: "https://www.youtube.com/watch?v=vid2", nativeId: "c2", at: T0 });
  attribution.set({ utteranceId: "v04", source: "niconico", sourceUrl: "https://www.nicovideo.jp/watch/sm1", nativeId: "n1", gameSlug: null, at: T0 });
  attribution.close();

  const tables = dumpTables(core);
  const queries: Array<{ terms: string[]; limit: number; useEmbedding: boolean }> = [
    { terms: ["スタークエストのボス戦は面白いか"], limit: 6, useEmbedding: false },
    { terms: ["Star Quest"], limit: 6, useEmbedding: false },
    { terms: [], limit: 6, useEmbedding: false },
    { terms: ["100%"], limit: 6, useEmbedding: false },
    { terms: ["ボス戦"], limit: 2, useEmbedding: false },
    { terms: ["ボス戦"], limit: 10, useEmbedding: false },
    { terms: ["ドラゴンサーガ"], limit: 6, useEmbedding: false },
    { terms: ["経験値"], limit: 1, useEmbedding: true },
    { terms: ["経験値"], limit: 3, useEmbedding: true },
    { terms: ["XP"], limit: 5, useEmbedding: true },
    { terms: ["ボス戦"], limit: 4, useEmbedding: true },
    { terms: ["ボス戦"], limit: 10, useEmbedding: true },
    { terms: ["スタークエストのボス戦"], limit: 6, useEmbedding: true },
    { terms: ["経験値"], limit: 0, useEmbedding: true },
    { terms: ["存在しない語"], limit: 6, useEmbedding: false },
  ];
  const results = queries.map((q) => ({
    ...q,
    searchTerms: null as unknown,
    rows: searchExternalVoiceRows(core, ws, q.terms, q.limit, () => undefined, { useEmbedding: q.useEmbedding }),
  }));
  const adapter = createDiscatierContextProvider(core);
  const adapterQueries = [
    { terms: ["スタークエストのボス戦"], limit: 6 },
    { terms: ["100%"], limit: 6 },
    { terms: [], limit: 6 },
    { terms: ["ボス戦"], limit: 0 },
  ];
  const adapterResults = adapterQueries.map((q) => ({
    ...q,
    voices: adapter.listRelevantExternalVoices!(ws, q.terms, q.limit),
  }));
  write("voice_search.json", {
    workspaceId: ws,
    model: "golden-model",
    tables,
    attributions: [
      { utteranceId: "v01", source: "youtube", sourceUrl: "https://www.youtube.com/watch?v=vid1&lc=c1", nativeId: "c1", gameSlug: "star-quest", at: T0 },
      { utteranceId: "v12", source: "youtube", sourceUrl: "https://www.youtube.com/watch?v=vid2", nativeId: "c2", gameSlug: null, at: T0 },
      { utteranceId: "v04", source: "niconico", sourceUrl: "https://www.nicovideo.jp/watch/sm1", nativeId: "n1", gameSlug: null, at: T0 },
    ],
    queries: results,
    adapter: adapterResults,
  });
  core.close();
}

// ───────────────────────── md exporter ─────────────────────────
{
  const dir = path.join(tmp, "md");
  fs.mkdirSync(dir, { recursive: true });
  const core = createCore(path.join(dir, "db.kuzu"), path.join(dir, "events.jsonl"));
  const raw = core.client.raw;
  core.repos.game.create({ id: "g1", workspaceId: ws, title: "Test Game", genre: "RPG" });
  core.repos.mechanic.create({ id: "m1", workspaceId: ws, gameId: "g1", name: "Cast Spell", description: "魔法を撃つ\n二行目: コロン" });
  raw.prepare("UPDATE mechanics SET intends = ?, intended_affect = ? WHERE id = ?").run("リソース管理の戦術判断 # 注意", "tension", "m1");
  core.repos.mechanic.create({ id: "m2", workspaceId: ws, name: "- dash" });
  core.repos.aesthetic.create({ id: "a1", workspaceId: ws, gameId: "g1", name: "中世ファンタジー", description: "石造城" });
  core.repos.aesthetic.create({ id: "a2", workspaceId: ws, name: "'quoted' name" });
  core.repos.designGap.create({ id: "gap1", workspaceId: ws, gameId: "g1", title: "魔法の学習曲線", description: "新規プレイヤーが MP 概念で躓く", status: "open" });
  raw.prepare("UPDATE design_gaps SET gap_in = ?, expected_affect = ?, observed_affect = ? WHERE id = ?").run("m1", "mastery", "confusion", "gap1");
  core.repos.designGap.create({ id: "gap2", workspaceId: ws, title: "only observed" });
  raw.prepare("UPDATE design_gaps SET observed_affect = ?, status = NULL WHERE id = ?").run("boredom", "gap2");
  core.repos.designGap.create({ id: "gap3", workspaceId: ws, title: 'title: "quoted" # hash', description: "line1\nline2" });
  core.repos.session.create({ id: "s1", workspaceId: ws, title: "design discussion", startedAt: T0, mode: "emotion", scene: "discord:g/c" });
  raw.prepare("UPDATE sessions SET ended_at = ? WHERE id = ?").run(T0 + 99_000, "s1");
  core.repos.session.create({ id: "s2", workspaceId: ws, title: "minimal", startedAt: T0 + 1 });
  core.repos.session.create({ id: "s3", workspaceId: ws, title: "scene only", startedAt: T0 + 2, scene: "web:room" });
  core.repos.utterance.create({ id: "u1", workspaceId: ws, sessionId: "s1", speakerId: "alice", rawContent: "MP の意味がわからない\n2行目: colon\n", postedAt: T0 + 10 });
  raw.prepare("UPDATE utterances SET normalized_content = ? WHERE id = ?").run("MPの意味が不明", "u1");
  core.repos.utterance.create({ id: "u2", workspaceId: ws, sessionId: "s1", rawContent: "同じ", normalizedContent: "同じ", postedAt: T0 + 20, respondsTo: "u1" });
  core.repos.utterance.create({ id: "u3", workspaceId: ws, sessionId: "s2", speakerId: "bob", rawContent: "", postedAt: T0 + 30 });
  core.repos.hypothesis.create({ id: "h1", workspaceId: ws, designGapId: "gap1", statement: "リソース管理の戦術判断", status: "validated" });
  raw.prepare("UPDATE hypotheses SET refs_json = ?, validated_by_emotion = 1, integrated = 1, stale_flagged = 1 WHERE id = ?").run(JSON.stringify(["u1"]), "h1");
  core.repos.hypothesis.create({ id: "h2", workspaceId: ws, statement: "- これは 32 文字を超える長い仮説の文で、先頭がハイフンになっている: yes" });
  raw.prepare("UPDATE hypotheses SET refs_json = ?, status = NULL WHERE id = ?").run("not json", "h2");
  core.repos.hypothesis.create({ id: "h3", workspaceId: ws, designGapId: "gap1", statement: "short", status: "proposed" });
  raw.prepare("UPDATE hypotheses SET refs_json = ? WHERE id = ?").run(JSON.stringify(["u2", 5, "u1"]), "h3");
  let n = 0;
  for (const t of ["games", "mechanics", "aesthetics", "design_gaps", "sessions", "utterances", "hypotheses"]) {
    for (const r of raw.prepare(`SELECT rowid AS rid FROM ${t} ORDER BY rowid`).all() as Array<{ rid: number }>) {
      n += 1;
      raw.prepare(`UPDATE ${t} SET created_at = ?, updated_at = ? WHERE rowid = ?`).run(T0 + n * 1000, T0 + n * 1000 + 500, r.rid);
    }
  }
  const tables = dumpTables(core);
  const cases: Array<[string, string]> = [
    ["hyp", "h1"], ["hyp", "h2"], ["hyp", "h3"], ["gap", "gap1"], ["gap", "gap2"], ["gap", "gap3"],
    ["mch", "m1"], ["mch", "m2"], ["aes", "a1"], ["aes", "a2"], ["utt", "u1"], ["utt", "u2"], ["utt", "u3"],
    ["ses", "s1"], ["ses", "s2"], ["ses", "s3"], ["hyp", "missing-id"], ["aff", "x"], ["game", "g1"], ["persona", "p"],
  ];
  const results = [];
  for (const [type, id] of cases) results.push({ type, id, result: await run(() => md.exportNode(core, type as never, ws, id)) });
  write("md_exporter.json", { workspaceId: ws, tables, cases: results });
  core.close();
}

// ───────────────────────── auto discussion ─────────────────────────
{
  const fallbackCases: Array<[string, string | null]> = [
    ["ヴァンサバに必要な要素とは何か？", null],
    ["/command arg", null],
    ["HTTP://example.com/x", null],
    ["短い?", null],
    ["こんにちは、今日は晴れです", null],
    ["このゲームのテンポは良いか悪いか議論したい", null],
    ["なぜ敵が強いのかな", "ワンダと巨像"],
    ["   なぜ敵が強いのかな   ", "  "],
    ["vampire survivors is fun", null],
    ["Vampire survivors is fun", null],
    ["どうして" + "長".repeat(120) + "🎮🎮", null],
    ["", null],
  ];
  const fallback = fallbackCases.map(([content, title]) => ({
    content,
    threadTitle: title,
    result: ad.classifyDiscordMessageFallback(content, title ?? undefined),
  }));
  const directives = ["improvement", "fun", "other"].map((d) => ({ direction: d, text: ad.forumDirectionDirective(d as never) }));

  const llmCases: Array<{ content: string; threadTitle: string | null; llm: { ok: boolean; text?: string } }> = [
    { content: "Vampire Survivors はテンポが悪いと思う", threadTitle: null, llm: { ok: true, text: JSON.stringify({ action: "start_discussion", category: "opinion", gameTitle: "Vampire Survivors", genre: "ローグライト", title: "LLM分類議題", description: "LLMで分類した議題", reason: "mock" }) } },
    { content: "なんか最近のゲームつまらないよね", threadTitle: null, llm: { ok: true, text: JSON.stringify({ action: "start_discussion", category: "opinion", gameTitle: "", title: "曖昧な議題", description: "どのゲームか不明", reason: "mock" }) } },
    { content: "なんか最近のゲームつまらないよね", threadTitle: null, llm: { ok: true, text: JSON.stringify({ action: "start_discussion", category: "opinion", gameTitle: "  " }) } },
    { content: "よじ登りがだるい", threadTitle: "ワンダと巨像", llm: { ok: true, text: "前置き\n```json\n" + JSON.stringify({ action: "start_discussion", category: "game_design_question", title: "全然違うゲームの議題", description: "本文だけから推測した説明" }) + "\n```\n後書き}" } },
    { content: "何かの投稿", threadTitle: null, llm: { ok: true, text: "[1, 2, {\"a\": 1}]" } },
    { content: "何かの投稿", threadTitle: null, llm: { ok: true, text: "no json here" } },
    { content: "何かの投稿?", threadTitle: null, llm: { ok: true, text: "{ broken json" } },
    { content: "何かの投稿?", threadTitle: null, llm: { ok: true, text: "} reversed {" } },
    { content: "何かの投稿?", threadTitle: "スレ", llm: { ok: false } },
    { content: "x", threadTitle: null, llm: { ok: true, text: JSON.stringify({ action: "start_discussion", category: "mystery", gameTitle: "G", title: "T".repeat(100) + "🎮", description: "D".repeat(700), expectedAffect: "  ", observedAffect: " joy ", reason: "  " }) } },
    { content: "x", threadTitle: null, llm: { ok: true, text: JSON.stringify({ action: "record_only", category: "noise", title: 5 }) } },
    { content: "x", threadTitle: null, llm: { ok: true, text: JSON.stringify({ action: "start_discussion", category: "opinion", gameTitle: null, title: null, genre: "ジャンル" }) } },
  ];
  const llm = [];
  for (const c of llmCases) {
    let lastArgs: unknown = null;
    const client = {
      async invoke(args: unknown) {
        lastArgs = args;
        return c.llm.ok ? { ok: true, text: c.llm.text } : { ok: false, error: "mock failure" };
      },
    };
    const result = await run(() => ad.classifyDiscordMessage(c.content, client as never, c.threadTitle ?? undefined));
    llm.push({ ...c, result, invocation: lastArgs });
  }

  const dir = path.join(tmp, "ad");
  fs.mkdirSync(dir, { recursive: true });
  const core = createCore(path.join(dir, "db.kuzu"), path.join(dir, "events.jsonl"));
  const startCases = [
    { input: { workspaceId: ws, guildId: "g1", channelId: "ch1", sessionId: "s1", utteranceId: "ua", authorId: "u1", content: "ヴァンサバに必要な要素とは何か？" }, llmText: null },
    { input: { workspaceId: ws, guildId: "g1", channelId: "ch1", sessionId: "s1", utteranceId: "ua", authorId: "u1", content: "ヴァンサバに必要な要素とは何か？" }, llmText: null },
    { input: { workspaceId: ws, guildId: "g1", channelId: "ch-forum", sessionId: "s1", utteranceId: "ub", authorId: "u1", content: "名作の要素はどこ？", forumTitle: " ワンダと巨像 ", direction: "improvement", forumTags: [" 議論 ", "", "開発"] }, llmText: JSON.stringify({ action: "start_discussion", category: "game_design_question", title: "全然違うゲームの議題", description: "本文だけから推測した説明", expectedAffect: "awe" }) },
    { input: { workspaceId: ws, guildId: "web", channelId: "room", sessionId: "s2", utteranceId: "uc", authorId: "u2", content: "このゲームのテンポは?", direction: "fun" }, llmText: JSON.stringify({ action: "start_discussion", category: "opinion", gameTitle: "Hollow Knight", genre: "メトロイドヴァニア", description: "", reason: "r" }) },
    { input: { workspaceId: ws, guildId: "g1", channelId: "ch2", sessionId: "s3", utteranceId: "ud", authorId: "u3", content: "天気の話" }, llmText: null },
  ];
  const starts = [];
  for (const c of startCases) {
    const client = c.llmText ? new MockLLMClient([() => c.llmText!]) : null;
    const r = await ad.startAutoDiscussionForDiscordMessage(core, c.input as never, client);
    const gap = r.gapId
      ? (core.client.raw.prepare("SELECT title, description, status, gap_in, expected_affect, observed_affect, evidence_json FROM design_gaps WHERE id = ?").get(r.gapId) as Record<string, unknown>)
      : null;
    if (gap) gap.gap_in = String(gap.gap_in).replace(r.gapId!, "<gapId>");
    starts.push({ ...c, started: r.started, hasGapId: Boolean(r.gapId), classification: r.classification, gap });
  }
  core.close();
  write("auto_discussion.json", { fallback, directives, llm, starts });
}

// ───────────────────────── command handlers ─────────────────────────
{
  const dir = path.join(tmp, "cmd");
  fs.mkdirSync(dir, { recursive: true });
  const core = createCore(path.join(dir, "db.kuzu"), path.join(dir, "events.jsonl"));
  const raw = core.client.raw;
  core.repos.mechanic.create({ id: "m1", workspaceId: ws, name: "Dash", description: "高速移動" });
  raw.prepare("UPDATE mechanics SET intends = ? WHERE id = ?").run("爽快感", "m1");
  core.repos.mechanic.create({ id: "m2", workspaceId: ws, name: "Jump" });
  core.repos.affect.create({ id: "af1", workspaceId: ws, mood: "Thrill" });
  core.repos.session.create({ id: "s1", workspaceId: ws, title: "play", startedAt: T0, mode: "theory" });
  core.repos.utterance.create({ id: "u1", workspaceId: ws, sessionId: "s1", rawContent: "Dash が気持ちいい", postedAt: T0 + 1 });
  core.repos.utterance.create({ id: "u2", workspaceId: ws, sessionId: "s1", rawContent: "  dash 遅い", postedAt: T0 + 2 });
  core.repos.utterance.create({ id: "u3", workspaceId: ws, sessionId: "s1", rawContent: "Jump\tもっと高く", postedAt: T0 + 3 });
  core.repos.utterance.create({ id: "u4", workspaceId: ws, sessionId: "s1", rawContent: "dash again", postedAt: T0 + 4 });
  raw.prepare("UPDATE utterances SET normalized_content = '[uncategorized] x' WHERE id IN ('u1','u2','u3','u4')").run();
  core.repos.designGap.create({ id: "gap1", workspaceId: ws, title: "Dash の爽快感" });
  raw.prepare("UPDATE design_gaps SET gap_in = 'm1' WHERE id = 'gap1'").run();
  core.repos.hypothesis.create({ id: "h1", workspaceId: ws, statement: "Dash を短くすると爽快感が増す", status: "draft" });
  core.repos.hypothesis.create({ id: "h2", workspaceId: ws, statement: "Jump は不要", status: "proposed" });
  raw.prepare("UPDATE hypotheses SET stale_flagged = 1, refs_json = '[\"u1\"]' WHERE id = 'h2'").run();
  core.repos.person.create({ id: "p1", workspaceId: ws, name: "Alice", role: "player" });
  let n = 0;
  for (const t of ["mechanics", "affects", "sessions", "utterances", "design_gaps", "hypotheses", "people"]) {
    for (const r of raw.prepare(`SELECT rowid AS rid FROM ${t} ORDER BY rowid`).all() as Array<{ rid: number }>) {
      n += 1;
      raw.prepare(`UPDATE ${t} SET created_at = ?, updated_at = ? WHERE rowid = ?`).run(T0 + n * 1000, T0 + n * 1000, r.rid);
    }
  }
  // h1 を最新にする (latestHypothesis の対象)
  raw.prepare("UPDATE hypotheses SET updated_at = ? WHERE id = 'h1'").run(T0 + 900_000);
  const tables = dumpTables(core);

  type Step = { family: string; command: string; args: string[]; personId?: string };
  const steps: Step[] = [
    { family: "cross", command: "hotspot", args: [] },
    { family: "cross", command: "cluster", args: [] },
    { family: "cross", command: "find", args: ["DASH"] },
    { family: "cross", command: "find", args: ["thr"] },
    { family: "cross", command: "find", args: ["  "] },
    { family: "cross", command: "find", args: ["zzz"] },
    { family: "cross", command: "stale", args: [] },
    { family: "cross", command: "lineage", args: ["m1"] },
    { family: "cross", command: "lineage", args: ["nope"] },
    { family: "cross", command: "lineage", args: [] },
    { family: "cross", command: "me", args: ["Role", "Theorist"], personId: "p1" },
    { family: "cross", command: "me", args: ["role", "developer"], personId: "p9" },
    { family: "cross", command: "me", args: ["role", "boss"], personId: "p1" },
    { family: "cross", command: "me", args: ["x"], personId: "p1" },
    { family: "cross", command: "me", args: ["role", "player"] },
    { family: "cross", command: "nope", args: [] },
    { family: "learning", command: "intends", args: ["緊張", "感"] },
    { family: "learning", command: "refine", args: ["新しい", "説明"] },
    { family: "learning", command: "compare", args: ["a"] },
    { family: "learning", command: "compare", args: ["a", "b"] },
    { family: "learning", command: "history", args: ["Dash"] },
    { family: "learning", command: "define", args: [] },
    { family: "synthesis", command: "refs", args: ["u1,", " u2 ,,u3"] },
    { family: "synthesis", command: "submit", args: [] },
    { family: "synthesis", command: "addresses", args: ["gap1"] },
    { family: "synthesis", command: "submit", args: [] },
    { family: "synthesis", command: "validate", args: ["Theory"] },
    { family: "synthesis", command: "validate", args: ["bogus"] },
    { family: "synthesis", command: "integrate", args: [] },
    { family: "synthesis", command: "reject", args: [] },
    { family: "synthesis", command: "jump", args: [] },
    { family: "synthesis", command: "propose", args: [] },
    { family: "emotion", command: "scene", args: ["夜の", "街"] },
    { family: "emotion", command: "scene", args: [] },
    { family: "emotion", command: "done", args: [] },
  ];
  const results = [];
  for (const s of steps) {
    const before = (raw.prepare("SELECT COUNT(*) AS c FROM events").get() as { c: number }).c;
    const ctx = { core, workspaceId: ws, sessionId: "s1", personId: s.personId, args: s.args };
    const result = await run(() => {
      if (s.family === "cross") return cross.handleCross(ctx, s.command);
      const mod = { learning, synthesis, emotion }[s.family] as Record<string, (c: typeof ctx) => unknown>;
      const fn = mod[`handle${s.command[0].toUpperCase()}${s.command.slice(1)}`];
      return fn(ctx);
    });
    const events = (raw.prepare("SELECT event_type, payload_json FROM events ORDER BY rowid LIMIT -1 OFFSET ?").all(before) as Array<{ event_type: string; payload_json: string }>).map((e) => ({
      eventType: e.event_type,
      payload: JSON.parse(e.payload_json),
    }));
    results.push({ ...s, result, events });
  }
  write("command_handlers.json", { workspaceId: ws, sessionId: "s1", tables, steps: results });
  core.close();
}

fs.rmSync(tmp, { recursive: true, force: true });
