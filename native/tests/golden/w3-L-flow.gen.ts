// w3-L 担当 (議論の進行 rounds エンジン・結論・結論の可視化と export) の Node 実装の入出力を記録する。
// 実行: npx tsx native/tests/golden/w3-L-flow.gen.ts → native/tests/fixtures/flow/w3l_golden.json
//
// director の golden: 偽の LLM (プロンプトの種類で決まった応答を返す) で 1 議論を最後まで回し、
// LLM への呼び出し列 (system / prompt / model) と応答、DB に残る全行 (時刻の列を除く)、ペーパー md、
// onUtterance / onVote の通知、log / warn、戻り値を記録する。C++ は記録した応答を順に返して同じものを作る。
// UUID (randomUUID) は出てきた順に <U1> <U2> ... へ置き換える (両側で同じ順に現れるので一致する)。
// 投票者は 1 人にする: 同票のくじは id (乱数の UUID) の昇順で引くため、同票があると Node と C++ で勝者が変わる。
import fs from "node:fs";
import os from "node:os";
import path from "node:path";

const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "w3l-golden-"));
process.env.DISCUTERE_CONFIG = path.join(tmp, "no-config.json");
process.env.DATABASE_PATH = path.join(tmp, "init.db");

const UUID_RE = /[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}/g;

class Normalizer {
  map = new Map<string, string>();
  str(s: string): string {
    return s.replace(UUID_RE, (m) => {
      let v = this.map.get(m);
      if (!v) {
        v = `<U${this.map.size + 1}>`;
        this.map.set(m, v);
      }
      return v;
    });
  }
  any(v: unknown): unknown {
    if (typeof v === "string") return this.str(v);
    if (Array.isArray(v)) return v.map((x) => this.any(x));
    if (v && typeof v === "object") {
      const o: Record<string, unknown> = {};
      for (const [k, x] of Object.entries(v as Record<string, unknown>)) o[this.str(k)] = this.any(x);
      return o;
    }
    return v;
  }
}

function lcg(seed: number) {
  let s = seed;
  return () => {
    s = (s * 1664525 + 1013904223) & 0xffffffff;
    return (s >>> 0) / 0x100000000;
  };
}

type Script = {
  facilitator: Array<{ ok: boolean; text?: string; error?: string }>;
  persona: Array<{ ok: boolean; text?: string; error?: string }>;
  vote: string[];
  summary: string[];
  aufhebung: string[];
  converge: Array<{ ok: boolean; text?: string; error?: string }>;
  refine: string[];
};

function pick<T>(list: T[], i: number): T {
  return list[Math.min(i, list.length - 1)];
}

/** プロンプトの種類で応答を決める偽の LLM。呼び出しは正規化して記録する。 */
function scriptedLlm(script: Script, norm: Normalizer) {
  const calls: unknown[] = [];
  const counters: Record<string, number> = {};
  const next = (k: string) => {
    const n = counters[k] ?? 0;
    counters[k] = n + 1;
    return n;
  };
  return {
    calls,
    llm: {
      async invoke(args: { system?: string; prompt: string; model?: string; maxTokens?: number }) {
        const p = args.prompt;
        let r: { ok: boolean; text?: string; error?: string };
        let kind: string;
        if (p.includes("価値軸") && p.includes("JSON 配列")) {
          kind = "personaSetup";
          const ids = [...new Set(p.match(UUID_RE) ?? [])];
          r = { ok: true, text: JSON.stringify(ids.map((id, i) => ({ personaId: id, valueAxis: `価値${i + 1}`, coreClaims: [`核${i + 1}`] }))) };
        } else if (p.includes("中立な審判")) {
          kind = "vote";
          r = { ok: true, text: pick(script.vote, next(kind)) };
        } else if (p.includes("このラウンドの議論を 2〜3 文で要約")) {
          kind = "summary";
          r = { ok: true, text: pick(script.summary, next(kind)) };
        } else if (p.includes("found=false")) {
          kind = "aufhebung";
          r = { ok: true, text: pick(script.aufhebung, next(kind)) };
        } else if (p.includes("締めのまとめ") || (args.system ?? "").includes("締めのまとめ")) {
          kind = "converge";
          r = pick(script.converge, next(kind));
        } else if (p.includes("ディスカッションペーパー編集者")) {
          kind = "refine";
          r = { ok: true, text: pick(script.refine, next(kind)) };
        } else if (p.startsWith("あなたは議論の進行役です。\nテーマ")) {
          kind = "facilitator";
          r = pick(script.facilitator, next(kind));
        } else {
          kind = "persona";
          r = pick(script.persona, next(kind));
        }
        calls.push({
          kind,
          system: args.system === undefined ? null : norm.str(args.system),
          prompt: norm.str(p),
          model: args.model ?? null,
          maxTokens: args.maxTokens ?? null,
          response: r.ok ? { ok: true, text: norm.str(r.text ?? "") } : { ok: false, error: r.error ?? "" },
        });
        return r.ok ? { ok: true as const, text: r.text ?? "" } : { ok: false as const, error: r.error ?? "" };
      },
    },
  };
}

const TIME_COL = /(_at|latency_ms)$/;

function dumpDb(dbPath: string, norm: Normalizer, Database: typeof import("better-sqlite3")) {
  const db = new Database(dbPath, { readonly: true });
  const tables = (db.prepare("SELECT name FROM sqlite_master WHERE type='table' ORDER BY name").all() as Array<{ name: string }>).map((t) => t.name);
  const out: Record<string, unknown[]> = {};
  for (const t of tables) {
    if (t === "_flow_migrations" || t === "sqlite_sequence") continue;
    const rows = db.prepare(`SELECT * FROM "${t}" ORDER BY rowid`).all() as Array<Record<string, unknown>>;
    if (rows.length === 0) continue;
    out[t] = rows.map((r) => {
      const o: Record<string, unknown> = {};
      for (const [k, v] of Object.entries(r)) if (!TIME_COL.test(k)) o[k] = v;
      return norm.any(o);
    });
  }
  db.close();
  return out;
}

async function main() {
  const Database = (await import("better-sqlite3")).default;
  const { _resetConfig } = await import("../../../src/config.js");
  const { _resetFlowDb } = await import("../../../src/flow/db/connection.js");
  const { runFlow } = await import("../../../src/flow/director.js");
  const { buildFacilitatorPrompt } = await import("../../../src/flow/facilitator-turn.js");
  const { buildRoundsSection } = await import("../../../src/flow/conclusion.js");
  const { insertPoolPersona } = await import("../../../src/flow/persona-pool.js");
  const { textToVector } = await import("../../../src/flow/sentiment-vector.js");
  const { renderConclusionMarkdown, safeSlug } = await import("../../../src/visualize/conclusion-markdown.js");
  const { buildDiscussionSnapshot } = await import("../../../src/fundamentum-export/snapshot.js");
  const { Foundation } = await import("fundamentum");
  const fc = await import("../../../src/visualize/flow-conclusions.js");
  const cc = await import("../../../src/visualize/conclusion-cache.js");
  const { applyFlowMigrations } = await import("../../../src/flow/db/migrations.js");
  const { listConclusions, getConclusionDetail } = await import("../../../src/visualize/conclusions.js");
  const { createCore } = await import("../../../src/core/index.js");

  const out: Record<string, unknown> = {};

  // ── facilitator-turn / conclusion の純関数 ──
  out.facilitatorPrompts = [
    { theme: "テーマA", round: 1, prev: null, issues: [] },
    { theme: "テーマB", round: 1, prev: null, issues: ["論点1", "論点2"] },
    { theme: "テーマC", round: 2, prev: { round: 1, summary: "まとめ", aufhebung: [], winner: null }, issues: [] },
    {
      theme: "テーマD",
      round: 3,
      prev: { round: 2, summary: "", aufhebung: ["止揚1", "止揚2"], winner: { personaName: "陽介", text: "あ".repeat(130) + "😀".repeat(5) } },
      issues: ["q"],
    },
  ].map((c) => ({ ...c, prompt: buildFacilitatorPrompt(c.theme, c.round, c.prev, c.issues) }));
  out.roundsSections = [
    [],
    [{ round: 1, summary: "s1", aufhebung: [] }],
    [
      { round: 1, summary: "s1", aufhebung: ["a", "b"] },
      { round: 2, summary: "", aufhebung: ["c"] },
    ],
  ].map((rounds) => ({ rounds, text: buildRoundsSection(rounds) }));

  // ── conclusion-markdown ──
  out.slugs = [
    ["ローグライトの周回設計", "x"],
    ["a/b:c*d?", "x"],
    ["", "fallback-id"],
    ["   ", "fb"],
    ["--a  b--", "fb"],
    ["#%{}\\|<>\"\t\n　x y", "fb"],
    ["あ".repeat(70), "fb"],
    ["😀".repeat(35), "fb"],
  ].map(([t, f]) => ({ title: t, fallback: f, slug: safeSlug(t, f) }));
  const baseDetail = {
    gapId: "flow:sess-1",
    kind: "flow",
    sessionId: "sess-1",
    title: "周回設計",
    conclusion: "ビルド多様性で飽きを防ぐ",
    utteranceCount: 2,
    aufhebungCount: 1,
    updatedAt: 1700000000000,
    paper: {
      theme: "周回設計",
      tags: ["開発", "内部"],
      supplement: "社外秘の数値には触れない。",
      mechanics: [
        { name: "ビルド選択", description: "周回ごとに構成を変える", intendedAffect: "発見" },
        { name: "難度上昇", description: "周回で敵が強くなる" },
      ],
    },
    aufhebungen: ["難度と報酬の対応を保つ"],
    topOpinions: [{ speaker: "ミナ (反対派)", content: "分岐で多様性", score: 2, source: null, sourceUrl: null }],
    transcript: [
      { speaker: "陽介 (賛成派)", content: "飽きが課題", source: null, sourceUrl: null },
      { speaker: "論者#abc", content: "外部の声", source: "youtube-livechat", sourceUrl: "https://youtu.be/x" },
      { speaker: "X", content: "出所のみ", source: "steam", sourceUrl: null },
    ],
  };
  const details = [
    baseDetail,
    { ...baseDetail, paper: null },
    { ...baseDetail, conclusion: null, aufhebungen: [], topOpinions: [], transcript: [] },
    {
      ...baseDetail,
      kind: "gap",
      gapId: "gap-1",
      title: "key: value # not comment",
      sessionId: "",
      updatedAt: 0,
      paper: { theme: "", tags: [], supplement: "", mechanics: [] },
    },
    { ...baseDetail, title: "'quoted' \"double\"\nmulti", updatedAt: 1, conclusion: "- list\n: colon" },
    { ...baseDetail, title: "true", sessionId: "123", updatedAt: 253402300799999 },
  ];
  out.markdown = details.map((d) => ({ detail: d, md: renderConclusionMarkdown(d as never) }));

  // ── fundamentum-export snapshot ──
  out.snapshots = details.map((d) => {
    const snap = buildDiscussionSnapshot(d as never);
    const fm = Foundation.inMemory();
    return { detail: d, snapshot: snap, contentId: null as string | null, fm };
  });
  for (const s of out.snapshots as Array<{ snapshot: unknown; contentId: string | null; fm?: InstanceType<typeof Foundation> }>) {
    s.contentId = (await s.fm!.master.put(s.snapshot as never)).id;
    delete s.fm;
  }

  // ── flow-conclusions / conclusion-cache (flow DB を一時ファイルで) ──
  {
    const db = new Database(path.join(tmp, "fc.db"));
    applyFlowMigrations(db);
    const seedSql = [
      `INSERT INTO discussion_paper (id, flow, session_id, theme, tags_json, mechanics_json, supplement, created_at, updated_at) VALUES ('p1', 'discussion', 'sess-1', 'ローグライトの周回設計', '["開発","内部",3]', '[{"name":"ビルド選択","description":"周回ごとに構成を変える","intended_affect":"発見"},{"name":"難度上昇"},{"name":""},7,{"name":"x","description":5,"intended_affect":1}]', '  社外秘の数値には触れない。 ', 1, 1)`,
      `INSERT INTO discussion_paper (id, flow, session_id, theme, tags_json, mechanics_json, supplement, created_at, updated_at) VALUES ('p3', 'discussion', 'sess-3', '壊れたJSON', 'not json', '{bad', '', 1, 1)`,
      `INSERT INTO flow_conclusion (session_id, paper_id, summary, aufhebung_json, top_utterance_ids_json, concluded, created_at) VALUES ('sess-1', 'p1', '【収束】  周回はビルド多様性で飽きを防ぐ ', '["難度と報酬の対応を保つ","メタ進行は控えめに"]', '["u2","u1"]', 1, 2000)`,
      `INSERT INTO flow_conclusion (session_id, paper_id, summary, aufhebung_json, top_utterance_ids_json, concluded, created_at) VALUES ('sess-2', 'p-missing', '(結論なし)', '[]', '[]', 0, 3000)`,
      `INSERT INTO flow_conclusion (session_id, paper_id, summary, aufhebung_json, top_utterance_ids_json, concluded, created_at) VALUES ('sess-3', 'p3', '【収束(未検証)】生テキスト', 'oops', 'oops', 1, 1500)`,
      `INSERT INTO flow_conclusion (session_id, paper_id, summary, aufhebung_json, top_utterance_ids_json, concluded, created_at) VALUES ('sess-4', 'p-none', '【収束】(結論なし)', '[1,"a"]', '[]', 1, 1000)`,
      `INSERT INTO flow_utterance (id, session_id, paper_id, round, turn, persona_id, persona_name, role, stance, text, is_error, possession_name, created_at) VALUES ('u1', 'sess-1', 'p1', 1, 1, 'a', '陽介', 'debater', 'pro', '周回前提なら飽きが課題', 0, NULL, 100)`,
      `INSERT INTO flow_utterance (id, session_id, paper_id, round, turn, persona_id, persona_name, role, stance, text, is_error, possession_name, created_at) VALUES ('u2', 'sess-1', 'p1', 1, 2, 'b', 'ミナ', 'opinion', 'opinion', 'ビルド分岐で多様性を出せる', 0, '論者#abc123', 110)`,
      `INSERT INTO flow_utterance (id, session_id, paper_id, round, turn, persona_id, persona_name, role, stance, text, is_error, possession_name, created_at) VALUES ('uErr', 'sess-1', 'p1', 1, 0, 'c', '進行役', 'facilitator', 'neutral', '(エラー)', 1, NULL, 120)`,
      `INSERT INTO flow_utterance (id, session_id, paper_id, round, turn, persona_id, persona_name, role, stance, text, is_error, possession_name, created_at) VALUES ('u3', 'sess-1', 'p1', 2, 0, 'c', '進行役', 'facilitator', 'neutral', '次へ', 0, NULL, 130)`,
      `INSERT INTO vote (session_id, round, voter_index, chosen_utterance_id, created_at) VALUES ('sess-1', 1, 0, 'u2', 130)`,
      `INSERT INTO vote (session_id, round, voter_index, chosen_utterance_id, created_at) VALUES ('sess-1', 1, 1, 'u2', 131)`,
      `INSERT INTO vote (session_id, round, voter_index, chosen_utterance_id, created_at) VALUES ('sess-1', 1, 2, 'u1', 132)`,
    ];
    for (const s of seedSql) db.exec(s);
    const r: Record<string, unknown> = { seedSql };
    r.list = fc.listFlowConclusions(db, 100);
    r.listLimited = fc.listFlowConclusions(db, 2);
    r.details = Object.fromEntries(["sess-1", "sess-2", "sess-3", "sess-4", "nope"].map((s) => [s, fc.getFlowConclusionDetail(db, s)]));
    r.ids = ["flow:sess-1", "gap-1", "flow:", "flo"].map((g) => ({ g, is: fc.isFlowConclusionId(g), sid: fc.flowSessionIdFromGapId(g) }));
    cc.ensureConclusionCacheFresh(db);
    r.cacheAfterFresh = cc.listCachedConclusions(db, 100);
    cc.writeThroughFlowConclusion(db, { sessionId: "sess-1", title: "新タイトル", conclusion: null, utteranceCount: 9, aufhebungCount: 4, updatedAt: 5000 });
    cc.upsertConclusionCache(db, { gapId: "gap-x", kind: "gap", sessionId: "s", title: "t", conclusion: "c", utteranceCount: 3, aufhebungCount: 0, updatedAt: 4000 }, { materialCount: 12, now: 1 });
    cc.recomputeMaterialCounts(db, (s) => (s.gapId === "flow:sess-2" ? Number.NaN : s.gapId === "flow:sess-3" ? -5 : s.utteranceCount * 10), { onlyMissing: true });
    r.cacheFinal = cc.listCachedConclusions(db, 100);
    r.cacheCount = cc.conclusionCacheCount(db);
    out.flowConclusions = r;
    db.close();
  }

  // ── conclusions.ts (KG の design_gaps 経路) ──
  {
    const core = createCore(path.join(tmp, "kg.sqlite"), path.join(tmp, "events.jsonl"));
    const raw = core.client.raw;
    const setupSql = [
      `CREATE TABLE IF NOT EXISTS aufhebung_stock (id TEXT PRIMARY KEY, gap_id TEXT NOT NULL, summary TEXT NOT NULL, created_at INTEGER NOT NULL)`,
      `CREATE TABLE IF NOT EXISTS opinion_scores (target_id TEXT PRIMARY KEY, target_kind TEXT NOT NULL, score INTEGER NOT NULL DEFAULT 0, updated_at INTEGER NOT NULL)`,
      `INSERT INTO design_gaps (id, workspace_id, title, status, created_at, updated_at) VALUES ('g1', 'ws', '旧議論1', 'closed', 1, 100)`,
      `INSERT INTO design_gaps (id, workspace_id, title, status, created_at, updated_at) VALUES ('g2', 'ws', '旧議論2', 'converged', 1, 200)`,
      `INSERT INTO design_gaps (id, workspace_id, title, status, created_at, updated_at) VALUES ('g3', 'ws', '進行中', 'open', 1, 300)`,
      `INSERT INTO design_gaps (id, workspace_id, title, status, created_at, updated_at) VALUES ('g4', 'ws', 'セッション無し', 'closed', 1, 50)`,
      `INSERT INTO design_gaps (id, workspace_id, title, status, created_at, updated_at) VALUES ('g5', 'other', '別WS', 'closed', 1, 400)`,
      `INSERT INTO sessions (id, workspace_id, title, started_at, created_at, updated_at) VALUES ('s1', 'ws', 'discussion-of-gap:g1', 10, 1, 1)`,
      `INSERT INTO sessions (id, workspace_id, title, started_at, created_at, updated_at) VALUES ('s2', 'ws', 'discussion-of-gap:g2', 20, 1, 1)`,
      `INSERT INTO sessions (id, workspace_id, title, started_at, created_at, updated_at) VALUES ('s2b', 'ws', 'discussion-of-gap:g2', 25, 1, 1)`,
      `INSERT INTO utterances (id, workspace_id, session_id, speaker_id, raw_content, posted_at, created_at, updated_at) VALUES ('x1', 'ws', 's1', 'persona:陽介', '意見1', 1, 1, 1)`,
      `INSERT INTO utterances (id, workspace_id, session_id, speaker_id, raw_content, posted_at, created_at, updated_at) VALUES ('x2', 'ws', 's1', 'ext:youtube:abc', '外部', 2, 1, 1)`,
      `INSERT INTO utterances (id, workspace_id, session_id, speaker_id, raw_content, posted_at, created_at, updated_at) VALUES ('x3', 'ws', 's1', NULL, '匿名', 3, 1, 1)`,
      `INSERT INTO utterances (id, workspace_id, session_id, speaker_id, raw_content, posted_at, created_at, updated_at) VALUES ('x4', 'ws', 's1', 'discord:123', '人', 4, 1, 1)`,
      `INSERT INTO utterances (id, workspace_id, session_id, speaker_id, raw_content, posted_at, created_at, updated_at) VALUES ('x5', 'ws', 's1', 'persona:ミナ', '【収束】  まとめ1 ', 5, 1, 1)`,
      `INSERT INTO utterances (id, workspace_id, session_id, speaker_id, raw_content, posted_at, created_at, updated_at) VALUES ('x6', 'ws', 's1', 'persona:B', '意見2', 6, 1, 1)`,
      `INSERT INTO utterances (id, workspace_id, session_id, speaker_id, raw_content, posted_at, created_at, updated_at) VALUES ('y1', 'ws', 's2b', 'persona:C', '【収束】まとめ2', 1, 1, 1)`,
      `INSERT INTO aufhebung_stock (id, gap_id, summary, created_at) VALUES ('a2', 'g1', '止揚B', 20)`,
      `INSERT INTO aufhebung_stock (id, gap_id, summary, created_at) VALUES ('a1', 'g1', '止揚A', 10)`,
      `INSERT INTO opinion_scores (target_id, target_kind, score, updated_at) VALUES ('x1', 'utterance', 2, 1)`,
      `INSERT INTO opinion_scores (target_id, target_kind, score, updated_at) VALUES ('x6', 'utterance', 5, 1)`,
      `INSERT INTO opinion_scores (target_id, target_kind, score, updated_at) VALUES ('x5', 'utterance', 0, 1)`,
    ];
    for (const s of setupSql) raw.exec(s);
    const attribution = {
      get(id: string) {
        return id === "x2" ? { source: "youtube", sourceUrl: "https://youtu.be/abc" } : id === "x6" ? { source: "steam", sourceUrl: "" } : undefined;
      },
    };
    out.kgConclusions = {
      setupSql,
      list: listConclusions(core, "ws", 20),
      listLimited: listConclusions(core, "ws", 1),
      detail: getConclusionDetail(core, "ws", "g1"),
      detailAttributed: getConclusionDetail(core, "ws", "g1", attribution as never),
      detailG2: getConclusionDetail(core, "ws", "g2"),
      detailNoSession: getConclusionDetail(core, "ws", "g4"),
      missing: getConclusionDetail(core, "ws", "nope"),
    };
    core.client.close?.();
  }

  // ── director (runFlow) ──
  const scenarios: Array<{
    name: string;
    env: Record<string, string>;
    theme: string;
    tags: string[];
    seed: number;
    options: Record<string, unknown>;
    voices: Array<{ content: string; source: string; sourceUrl?: string }>;
    script: Script;
    evaluator?: "scores";
    onVoteThrows?: boolean;
    poolPersona?: boolean;
  }> = [
    {
      name: "basic",
      env: { DISCUTERE_FLOW_ROUNDS: "2", DISCUTERE_FLOW_TURNS_PER_ROUND: "3", DISCUTERE_FLOW_PERSONA_COUNT: "3", DISCUTERE_FLOW_VOTER_COUNT: "1" },
      theme: "ジャーニーの感情設計",
      tags: [],
      seed: 1234,
      options: { sessionId: "golden-basic" },
      voices: [
        { content: "ユーザの声1", source: "youtube", sourceUrl: "https://youtu.be/v1" },
        { content: "ユーザの声2", source: "steam" },
      ],
      script: {
        facilitator: [{ ok: true, text: "  開幕の議題です  " }, { ok: true, text: "次のラウンドの議題" }],
        persona: [{ ok: true, text: "意見1" }, { ok: true, text: "   " }, { ok: true, text: "意見3" }, { ok: true, text: "意見4" }],
        vote: ["2", "わからない"],
        summary: ["ラウンドの要約1", "ラウンドの要約2"],
        aufhebung: ['{"found": true, "summary": "止揚その1"}', '{"found": false, "summary": ""}'],
        converge: [{ ok: true, text: '前置き {"summary": "ビルド多様性で飽きを防ぐ"} 後置き' }],
        refine: [],
      },
    },
    {
      name: "override",
      env: {
        DISCUTERE_FLOW_ROUNDS: "5",
        DISCUTERE_FLOW_TURNS_PER_ROUND: "4",
        DISCUTERE_FLOW_PERSONA_COUNT: "4",
        DISCUTERE_FLOW_VOTER_COUNT: "1",
        DISCUTERE_FACILITATOR_AUFHEBUNG_TARGET: "1",
        DISCUTERE_FLOW_CONVERGE_SHARE: "0.5",
        DISCUTERE_FLOW_PAPER_REFINE_ENABLED: "true",
        DISCUTERE_FLOW_PAPER_REFINE_MODEL: "refine-model",
      },
      theme: "ガチャの射幸性",
      tags: ["機密", "開発"],
      seed: 99,
      options: {
        sessionId: "golden-override",
        rounds: 3.7,
        turnsPerRound: 2,
        paperOverride: {
          mechanics: [
            { name: "ガチャ", description: "ランダム排出", intended_affect: "期待" },
            { name: "天井", description: "一定回数で確定" },
          ],
          supplement: "社外秘の数値は出さない",
          bodyMd: "# 議題\nガチャの射幸性\n\n# 観点補足\n社外秘の数値は出さない\n\n# ゲームのメカニクス\n- **ガチャ**: ランダム排出 → 期待感情: 期待\n- **天井**: 一定回数で確定",
          issues: [" 論点A ", "", "論点B", "c", "d", "e", "f"],
          debatability: { debatable: false, armableBothCount: 1 },
        },
      },
      voices: [],
      script: {
        facilitator: [{ ok: false, error: "API timeout" }, { ok: true, text: "R2 の議題" }],
        persona: [{ ok: true, text: "賛成です" }, { ok: false, error: "rate limited" }, { ok: true, text: "反対です" }],
        vote: ["1", "2"],
        summary: ["要約A", "要約B"],
        aufhebung: ['{"found": false}', '{"found": true, "summary": "射幸性と天井の両立"}'],
        converge: [{ ok: true, text: "JSON ではない結論" }, { ok: true, text: "  それでも JSON ではない  " }],
        refine: ["# 議題\nガチャの射幸性 (改訂)\n\n# 観点補足\n改訂補足\n\n# ゲームのメカニクス\n- **ガチャ**: 改訂 → 期待感情: 期待\n- **新規**: 追加"],
      },
    },
    {
      name: "scores",
      env: { DISCUTERE_FLOW_ROUNDS: "3", DISCUTERE_FLOW_TURNS_PER_ROUND: "2", DISCUTERE_FLOW_PERSONA_COUNT: "3", DISCUTERE_FACILITATOR_AUFHEBUNG_TARGET: "1" },
      theme: "ローグライトの中毒性はどこから来るか",
      tags: ["運営"],
      seed: 7,
      options: { sessionId: "golden-scores" },
      voices: [],
      evaluator: "scores",
      onVoteThrows: true,
      poolPersona: true,
      script: {
        facilitator: [{ ok: true, text: "" }],
        persona: [{ ok: true, text: "中毒性は報酬間隔から" }],
        vote: ["1"],
        summary: ["要約S"],
        aufhebung: ['{"found": true, "summary": "報酬間隔と選択の両輪"}'],
        converge: [{ ok: false, error: "converge down" }],
        refine: [],
      },
    },
  ];

  const ALL_ENV = [
    "DISCUTERE_FLOW_ROUNDS",
    "DISCUTERE_FLOW_TURNS_PER_ROUND",
    "DISCUTERE_FLOW_PERSONA_COUNT",
    "DISCUTERE_FLOW_VOTER_COUNT",
    "DISCUTERE_FACILITATOR_AUFHEBUNG_TARGET",
    "DISCUTERE_FLOW_CONVERGE_SHARE",
    "DISCUTERE_FLOW_PAPER_REFINE_ENABLED",
    "DISCUTERE_FLOW_PAPER_REFINE_MODEL",
  ];
  const runs: unknown[] = [];
  for (const sc of scenarios) {
    for (const k of ALL_ENV) delete process.env[k];
    for (const [k, v] of Object.entries(sc.env)) process.env[k] = v;
    const dbPath = path.join(tmp, `${sc.name}.db`);
    process.env.DATABASE_PATH = dbPath;
    _resetConfig();
    _resetFlowDb();
    const gamesDir = path.join(tmp, `games-${sc.name}`);
    fs.mkdirSync(gamesDir, { recursive: true });
    let poolVector: number[] | null = null;
    if (sc.poolPersona) {
      poolVector = textToVector(sc.theme);
      insertPoolPersona({
        id: "pool-possess-1",
        name: "論者#test01",
        role: "opinion",
        speechStyle: "",
        traits: [],
        affectVector: poolVector,
        origin: "adopted",
        parentIds: [],
        learningSource: "youtube",
        sourceSpeakerId: "ext:youtube:tester",
        typicality: 0.95,
        polarityBias: 0.6,
        affectDispersion: 0.2,
      } as never);
    }
    const norm = new Normalizer();
    const { llm, calls } = scriptedLlm(sc.script, norm);
    const logs: string[] = [];
    const warns: string[] = [];
    const utterEvents: unknown[] = [];
    const voteEvents: unknown[] = [];
    const lookups: unknown[] = [];
    const options: Record<string, unknown> = {
      ...sc.options,
      llm,
      rng: lcg(sc.seed),
      gamesDir,
      listExternalVoices: (terms: string[], limit: number) => {
        lookups.push({ terms, limit });
        return sc.voices;
      },
      log: (m: string) => logs.push(norm.str(m)),
      warn: (m: string) => warns.push(norm.str(m)),
      onUtterance: (u: unknown) => {
        utterEvents.push(norm.any(u));
      },
      onVote: (e: unknown) => {
        voteEvents.push(norm.any(e));
        if (sc.onVoteThrows) throw new Error("vote sink down");
      },
    };
    if (sc.evaluator === "scores") {
      options.flow = "improvement";
      options.evaluateRound = async (ctx: { round: number; candidates: Array<{ id: string }> }) => {
        const tally: Record<string, number> = {};
        ctx.candidates.forEach((c, i) => (tally[c.id] = 0.5 - i * 0.25));
        return { kind: "scores", tally, winner: ctx.candidates[0]?.id ?? null, winnerShare: null };
      };
    }
    const result = await runFlow(sc.theme, sc.tags as never, options as never);
    _resetFlowDb();
    runs.push({
      name: sc.name,
      env: sc.env,
      theme: sc.theme,
      tags: sc.tags,
      seed: sc.seed,
      options: sc.options,
      voices: sc.voices,
      evaluator: sc.evaluator ?? null,
      onVoteThrows: !!sc.onVoteThrows,
      poolVector,
      calls,
      result: norm.any(result),
      utterEvents,
      voteEvents,
      lookups,
      logs,
      warns,
      db: dumpDb(dbPath, norm, Database),
    });
  }
  out.runs = runs;

  const dest = path.resolve("native/tests/fixtures/flow/w3l_golden.json");
  fs.writeFileSync(dest, JSON.stringify(out, null, 1) + "\n");
  console.log(`wrote ${dest}`);
  fs.rmSync(tmp, { recursive: true, force: true });
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
