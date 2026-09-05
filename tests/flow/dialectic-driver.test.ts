/**
 * dialectic 位相ドライバ (respec 05/06 / PR-B) E2E テスト (MockLLM)。
 * - [0]→[5] が完走し、4 表 (flow_issue/flow_position/flow_tension/flow_synthesis) に整合した
 *   レコードが残る + act/target が flow_utterance に永続される
 * - 事実対立: Mock 証拠で resolved / 証拠なしで unresolved_fact + 結論文に「未解決の事実問題」
 * - 批准: reject → 修正ループ → 2 回で打ち切り agreed_disagree
 * - elevates 空 → kind=compromise (止揚ストック = flow_conclusion.aufhebung_json に数えない)
 * - dispatch: flow.engine="dialectic" で discussion が dialectic 経路に分岐する
 */

import assert from "node:assert/strict";
import path from "node:path";
import fs from "node:fs";
import Database from "better-sqlite3";

const TMP_DIR = path.resolve(".tmp/flow-dialectic-driver-test");
fs.rmSync(TMP_DIR, { recursive: true, force: true });
fs.mkdirSync(TMP_DIR, { recursive: true });

const DB_PATH = path.join(TMP_DIR, "test.db");
process.env.DATABASE_PATH = DB_PATH;
process.env.DISCUTERE_FLOW_PERSONA_COUNT = "4";
process.env.DISCUTERE_FLOW_VOTER_COUNT = "3";
process.env.DISCUTERE_FLOW_ROSTER_FACILITATOR = "gpt-facilitator@high";
process.env.DISCUTERE_FLOW_ROSTER_DISCUSSANTS = "claude-pro@high,gpt-con@medium,claude-opinion@low";
process.env.DISCUTERE_FLOW_DIALECTIC_JUDGE_MODEL = "claude-judge@low";

const { _resetFlowDb } = await import("../../src/flow/db/connection.js");
const { _resetConfig } = await import("../../src/config.js");
_resetFlowDb();
_resetConfig();

const { runDialecticFlow } = await import("../../src/flow/dialectic/driver.js");
const { dispatchFlow } = await import("../../src/flow/dispatch.js");

// ── ルーター Mock (呼び出し順に依存しない・プロンプトのマーカーで応答を決める) ──

type Route = { name: string; match: (p: string) => boolean; respond: (p: string) => string };

function extractGroundIds(prompt: string): string[] {
  const ids = new Set<string>();
  for (const m of prompt.matchAll(/\[((?:pro|con)-G\d+)\]/g)) ids.add(m[1]);
  return [...ids];
}

function extractOwnGroundIds(prompt: string): string[] {
  const m = prompt.match(/根拠は \[([^\]]+)\] だった/);
  return m ? m[1].split(",").map((s) => s.trim()).filter(Boolean) : [];
}

function makeRouterLlm(overrides: Partial<Record<string, (p: string) => string>> = {}) {
  const stats: Record<string, number> = {};
  const prompts: Record<string, string[]> = {};
  const models: Record<string, Array<string | undefined>> = {};
  const routes: Route[] = [
    { name: "persona-setup", match: (p) => p.includes("重視する価値 (価値軸)"), respond: () => "[]" },
    {
      name: "issue-decompose",
      match: (p) => p.includes("賛否が本気で割れる論点"),
      respond: () => '["論点その1は妥当か?", "論点その2は妥当か?", "論点その3は妥当か?"]',
    },
    {
      name: "position",
      match: (p) => p.includes("定立 (Position) を張ります"),
      respond: (p) =>
        JSON.stringify({
          claim: p.includes("(pro)") ? "賛成の主張" : "反対の主張",
          grounds: ["根拠その1", "根拠その2"],
          values: ["価値前提"],
          text: p.includes("(pro)") ? "賛成だよ、理由は2つある。" : "反対だね、こっちも理由がある。",
        }),
    },
    {
      name: "facilitator",
      match: (p) => p.includes("あなたは議論の進行役です。次の進行内容"),
      respond: () => "では続けましょう。",
    },
    {
      name: "turn-respond",
      match: (p) => p.includes("あなたの発言は次の JSON 1 個だけで返す") && p.includes("許可: support | concede | rebut | question"),
      respond: () => JSON.stringify({ act: "support", target: null, text: "いや、その反論にはこう返せる。" }),
    },
    {
      name: "turn-attack",
      match: (p) => p.includes("あなたの発言は次の JSON 1 個だけで返す"),
      respond: () => JSON.stringify({ act: "rebut", target: null, text: "その根拠は怪しいと思う。" }),
    },
    { name: "tension-classify", match: (p) => p.includes("この対立の「型」を 1 つだけ選んで"), respond: () => "values" },
    { name: "fact-resolve", match: (p) => p.includes("証拠はどちらの主張を支持しますか"), respond: () => "A" },
    {
      name: "synthesize",
      match: (p) => p.includes("止揚 (aufheben) の 3 要件"),
      respond: (p) =>
        JSON.stringify({
          text: "コア層は現状維持、ライト層は緩和という条件分割でいこう。",
          preserves: extractGroundIds(p),
          negates: [],
          elevates: "コア層とライト層で適用条件を分割する新しい枠",
        }),
    },
    { name: "elevation-gate", match: (p) => p.includes("統合案の「新しい枠」"), respond: () => "aufheben" },
    {
      name: "ratify",
      match: (p) => p.includes("チェックリスト (敵対的批准"),
      respond: (p) =>
        JSON.stringify({
          verdict: "accept",
          preservedIds: extractOwnGroundIds(p),
          missing: [],
          reason: "根拠が保存されている",
        }),
    },
    {
      name: "vote",
      match: (p) => p.includes("あなたは中立な審判として"),
      respond: (p) => {
        const ids = [...p.matchAll(/\[ID:([^\]]+)\]/g)].map((m) => m[1]).sort();
        return ids[0] ?? "";
      },
    },
    { name: "conclusion", match: (p) => p.includes('"summary"'), respond: () => '{"summary": "議論の到達点まとめ"}' },
  ];
  return {
    stats,
    prompts,
    models,
    async invoke(args: { prompt: string; system?: string; model?: string }) {
      const p = args.prompt;
      for (const r of routes) {
        if (r.match(p)) {
          stats[r.name] = (stats[r.name] ?? 0) + 1;
          (prompts[r.name] ??= []).push(p);
          (models[r.name] ??= []).push(args.model);
          const responder = overrides[r.name] ?? r.respond;
          return { ok: true as const, text: responder(p) };
        }
      }
      stats.unmatched = (stats.unmatched ?? 0) + 1;
      return { ok: true as const, text: "(unmatched)" };
    },
  };
}

function makeRng(seedInit: number) {
  let seed = seedInit;
  return () => {
    seed = (seed * 1664525 + 1013904223) & 0xffffffff;
    return (seed >>> 0) / 0x100000000;
  };
}

const db = () => new Database(DB_PATH);

// ── シナリオ 1: [0]→[5] 完走 + 4 表整合 + 止揚批准 + 計測型収束 ────────────────

{
  const llm = makeRouterLlm();
  const result = await runDialecticFlow("ガチャ緩和の是非", [], {
    llm,
    rng: makeRng(1234),
    gamesDir: path.join(TMP_DIR, "no-games-dir"),
    sessionId: "dlx-basic",
    log: () => {},
    warn: () => {},
  });

  assert.equal(llm.stats.unmatched ?? 0, 0, "未ルーティングの LLM 呼び出しなし");
  assert.ok(
    ["issue-decompose", "facilitator", "synthesize"].every((route) =>
      (llm.models[route] ?? []).every((model) => model === "gpt-facilitator@high")
    ),
    "論点分解・進行文・止揚生成は roster.facilitator を使う"
  );
  assert.ok(
    ["tension-classify", "elevation-gate", "ratify"].every((route) =>
      (llm.models[route] ?? []).every((model) => model === "claude-judge@low")
    ),
    "分類・折衷ゲート・批准は judgeModel を使う"
  );
  assert.equal(result.sessionId, "dlx-basic");
  assert.ok(result.concluded, "結論が concluded=true");
  assert.ok(result.conclusion.startsWith("【収束】"), "結論が【収束】形式");
  assert.equal(result.rounds, 1, "計測型収束 (2/3) で論点 1 件目で早期収束");

  const d = db();
  const issues = d.prepare("SELECT * FROM flow_issue WHERE session_id = ? ORDER BY ordinal").all("dlx-basic") as any[];
  assert.equal(issues.length, 3, "論点分解で 3 issue が永続される");
  assert.equal(issues[0].status, "concluded", "決着済み issue は concluded");
  assert.equal(issues[1].status, "open", "早期収束で打ち切られた issue は open のまま");
  assert.equal(issues[0].source, "llm", "LLM 分解の source 記録");

  const positions = d
    .prepare("SELECT * FROM flow_position WHERE issue_id = ? ORDER BY created_at, id")
    .all(issues[0].id) as any[];
  assert.equal(positions.length, 2, "issue 1 に Position 2 件 (pro/con)");
  assert.deepEqual(positions.map((p) => p.stance).sort(), ["con", "pro"], "pro/con が揃う");
  const proGrounds = JSON.parse(positions.find((p) => p.stance === "pro")!.grounds_json);
  assert.equal(proGrounds.length, 2, "grounds 2 件");
  assert.ok(
    proGrounds.every((g: any) => g.state === "defended"),
    `攻撃→防御の遷移が grounds_json に write-through される (got ${JSON.stringify(proGrounds)})`
  );

  const tensions = d.prepare("SELECT * FROM flow_tension WHERE issue_id = ?").all(issues[0].id) as any[];
  assert.equal(tensions.length, 1, "Tension 1 件");
  assert.equal(tensions[0].type, "values", "型分類が永続される");
  assert.equal(tensions[0].status, "synthesized", "批准済み止揚 → synthesized");

  const syntheses = d.prepare("SELECT * FROM flow_synthesis WHERE tension_id = ?").all(tensions[0].id) as any[];
  assert.equal(syntheses.length, 1, "Synthesis 1 行 (修正は同一行上書き)");
  assert.equal(syntheses[0].kind, "aufheben", "elevation ゲート通過 → aufheben");
  assert.equal(syntheses[0].status, "ratified", "両陣営 accept → ratified");
  assert.equal(syntheses[0].revision, 0, "修正なし → revision 0");
  const ratification = JSON.parse(syntheses[0].ratification_json);
  assert.equal(ratification.length, 2, "批准は両陣営ぶん 2 件");
  assert.ok(ratification.every((r: any) => r.verdict === "accept"), "両 accept");
  assert.ok(
    ratification.every((r: any) => r.preservedIds.length > 0),
    "保存確認された根拠 ID が記録される"
  );

  const utterances = d
    .prepare("SELECT * FROM flow_utterance WHERE session_id = ? ORDER BY created_at, turn")
    .all("dlx-basic") as any[];
  const acts = utterances.map((u) => u.act).filter(Boolean);
  assert.ok(acts.includes("claim"), "act=claim が永続される");
  assert.ok(acts.includes("rebut"), "act=rebut が永続される");
  assert.ok(acts.includes("support"), "act=support が永続される");
  assert.ok(acts.includes("synthesize"), "act=synthesize が永続される");
  const rebutRow = utterances.find((u) => u.act === "rebut");
  assert.ok(rebutRow.target_id, "rebut の target_id が永続される");
  const facilitatorRows = utterances.filter((u) => u.role === "facilitator" && !u.act);
  assert.ok(facilitatorRows.length > 0, "ファシリテーター進行文は act なし");

  const conclusion = d.prepare("SELECT * FROM flow_conclusion WHERE session_id = ?").get("dlx-basic") as any;
  assert.ok(conclusion, "flow_conclusion に永続 (既存 generateConclusion 流用)");
  assert.equal(conclusion.concluded, 1);
  assert.equal(JSON.parse(conclusion.aufhebung_json).length, 1, "批准済み aufheben が 1 件");

  const paper = d.prepare("SELECT * FROM discussion_paper WHERE session_id = ?").get("dlx-basic") as any;
  assert.ok(paper, "discussion_paper が永続 (persistPaper 流用)");
  assert.ok(paper.body_md.includes("議論の経過") || paper.body_md.includes("論点"), "ペーパー本文に経過が焼かれる");

  const locations = new Set(
    (d.prepare("SELECT DISTINCT location FROM llm_call_log WHERE session_id = ?").all("dlx-basic") as any[]).map(
      (r) => r.location
    )
  );
  for (const loc of [
    "issue-decompose",
    "position",
    "utterance",
    "tension-classify",
    "synthesize",
    "elevation-gate",
    "ratify",
    "facilitator",
    "vote",
    "summary",
  ]) {
    assert.ok(locations.has(loc), `withCostLog location "${loc}" が記録される`);
  }
  d.close();
  console.log("  [ok] dialectic driver: [0]→[5] 完走・4 表整合・計測型収束・コストログ");
}

// ── シナリオ 2: 事実対立 — 証拠なし → unresolved_fact + 結論文に明記 ───────────

{
  const llm = makeRouterLlm({ "tension-classify": () => "facts" });
  const result = await runDialecticFlow("売上は下がるのか", [], {
    llm,
    rng: makeRng(42),
    gamesDir: path.join(TMP_DIR, "no-games-dir"),
    sessionId: "dlx-fact-unresolved",
    listExternalVoices: () => [],
    log: () => {},
    warn: () => {},
  });

  const d = db();
  const issue = d
    .prepare("SELECT * FROM flow_issue WHERE session_id = ? AND status = 'concluded'")
    .get("dlx-fact-unresolved") as any;
  const tension = d.prepare("SELECT * FROM flow_tension WHERE issue_id = ?").get(issue.id) as any;
  assert.equal(tension.status, "unresolved_fact", "証拠なし → unresolved_fact");
  assert.ok(tension.resolution_note.includes("未解決"), "resolution_note に未解決の記述");
  const syntheses = d.prepare("SELECT * FROM flow_synthesis WHERE tension_id = ?").all(tension.id) as any[];
  assert.equal(syntheses.length, 0, "事実対立は止揚の対象外 (Synthesis を作らない)");
  d.close();

  assert.ok(
    result.conclusion.includes("未解決の事実問題"),
    `結論文に「未解決の事実問題」が必ず明記される (got: ${result.conclusion.slice(0, 120)})`
  );
  assert.equal(llm.stats["fact-resolve"] ?? 0, 0, "証拠 0 件なら判定 LLM を呼ばない");
  console.log("  [ok] dialectic driver: 事実対立 (証拠なし) → unresolved_fact + 結論に明記");
}

// ── シナリオ 3: 事実対立 — Mock 証拠 + 判定 A 支持 → resolved ──────────────────

{
  const llm = makeRouterLlm({ "tension-classify": () => "facts", "fact-resolve": () => "A" });
  const result = await runDialecticFlow("売上は下がるのか (証拠あり)", [], {
    llm,
    rng: makeRng(7),
    gamesDir: path.join(TMP_DIR, "no-games-dir"),
    sessionId: "dlx-fact-resolved",
    listExternalVoices: () => [
      { content: "緩和後も売上は維持されたという決算資料の話", source: "niconico" },
      { content: "実際 DAU が伸びて回復した", source: "youtube" },
    ],
    log: () => {},
    warn: () => {},
  });

  const d = db();
  const issue = d
    .prepare("SELECT * FROM flow_issue WHERE session_id = ? AND status = 'concluded'")
    .get("dlx-fact-resolved") as any;
  const tension = d.prepare("SELECT * FROM flow_tension WHERE issue_id = ?").get(issue.id) as any;
  assert.equal(tension.status, "resolved", "証拠 + 判定で resolved");
  assert.ok(tension.resolution_note.includes("A を支持"), "解消内容が resolution_note に残る");
  d.close();

  assert.equal(llm.stats["fact-resolve"], 1, "判定 LLM が 1 回呼ばれる");
  assert.ok(!result.conclusion.includes("未解決の事実問題"), "解消済みなら未解決の明記はない");
  console.log("  [ok] dialectic driver: 事実対立 (Mock 証拠) → resolved");
}

// ── シナリオ 4: 批准 reject → 修正ループ → 2 回で打ち切り agreed_disagree ──────

{
  const llm = makeRouterLlm({
    ratify: (p) =>
      JSON.stringify({
        verdict: "reject",
        preservedIds: [],
        missing: extractOwnGroundIds(p),
        reason: "自分の根拠が保存されていない",
      }),
  });
  await runDialecticFlow("批准が割れるテーマ", [], {
    llm,
    rng: makeRng(99),
    gamesDir: path.join(TMP_DIR, "no-games-dir"),
    sessionId: "dlx-reject",
    log: () => {},
    warn: () => {},
  });

  const d = db();
  const issue = d
    .prepare("SELECT * FROM flow_issue WHERE session_id = ? AND status = 'concluded'")
    .get("dlx-reject") as any;
  const tension = d.prepare("SELECT * FROM flow_tension WHERE issue_id = ?").get(issue.id) as any;
  assert.equal(tension.status, "agreed_disagree", "批准不成立 → agreed_disagree (合意不能)");
  const synthesis = d.prepare("SELECT * FROM flow_synthesis WHERE tension_id = ?").get(tension.id) as any;
  assert.equal(synthesis.status, "rejected", "Synthesis は rejected で残る");
  assert.equal(synthesis.revision, 2, "修正ループはコードが最大 2 回でカット");
  const conclusion = d.prepare("SELECT * FROM flow_conclusion WHERE session_id = ?").get("dlx-reject") as any;
  assert.equal(JSON.parse(conclusion.aufhebung_json).length, 0, "reject された候補は止揚ストックに乗らない");
  d.close();

  assert.equal(llm.stats.synthesize, 3, "生成は初回 + 修正 2 回 = 3 回");
  assert.equal(llm.stats.ratify, 6, "批准は 2 陣営 × 3 周 = 6 回");
  assert.ok(
    (llm.prompts.synthesize ?? []).some((p) => p.includes("批准フィードバック")),
    "修正ループで missing がフィードバックされる"
  );
  assert.ok(
    (llm.prompts.synthesize ?? []).some((p) => p.includes("前回候補の言い換えは禁止")),
    "修正ループは前回案の反復を禁止する"
  );
  console.log("  [ok] dialectic driver: 批准 reject → 修正 2 回で打ち切り → agreed_disagree");
}

// ── シナリオ 5: elevates 空 → kind=compromise (止揚ストックに数えない) ─────────

{
  const llm = makeRouterLlm({
    synthesize: (p) =>
      JSON.stringify({
        text: "両方の意見をそのまま採用しよう。",
        preserves: extractGroundIds(p),
        negates: [],
        elevates: "",
      }),
  });
  await runDialecticFlow("折衷で終わるテーマ", [], {
    llm,
    rng: makeRng(555),
    gamesDir: path.join(TMP_DIR, "no-games-dir"),
    sessionId: "dlx-compromise",
    log: () => {},
    warn: () => {},
  });

  const d = db();
  const issue = d
    .prepare("SELECT * FROM flow_issue WHERE session_id = ? AND status = 'concluded'")
    .get("dlx-compromise") as any;
  const tension = d.prepare("SELECT * FROM flow_tension WHERE issue_id = ?").get(issue.id) as any;
  assert.equal(tension.status, "compromised", "折衷の批准 → compromised (synthesized ではない)");
  const synthesis = d.prepare("SELECT * FROM flow_synthesis WHERE tension_id = ?").get(tension.id) as any;
  assert.equal(synthesis.kind, "compromise", "elevates 空 → kind=compromise");
  assert.equal(synthesis.status, "ratified", "折衷でも批准は通る (記録はする)");
  const conclusion = d.prepare("SELECT * FROM flow_conclusion WHERE session_id = ?").get("dlx-compromise") as any;
  assert.equal(
    JSON.parse(conclusion.aufhebung_json).length,
    0,
    "折衷は止揚ストック (収束シグナル) に数えない"
  );
  d.close();

  assert.equal(llm.stats["elevation-gate"] ?? 0, 0, "elevates 空はコードのみで折衷判定 (LLM 不要)");
  console.log("  [ok] dialectic driver: elevates 空 → compromise 格下げ + 止揚ストック除外");
}

// ── シナリオ 6: dispatch の engine 分岐 (flow.engine=dialectic → dialectic 経路) ──

{
  process.env.DISCUTERE_FLOW_ENGINE = "dialectic";
  _resetConfig();

  const llm = makeRouterLlm();
  const result = await dispatchFlow(
    { theme: "エンジン分岐テスト", tags: [], flow: "議論" },
    {
      llm,
      rng: makeRng(2026),
      gamesDir: path.join(TMP_DIR, "no-games-dir"),
      sessionId: "dlx-dispatch",
      log: () => {},
      warn: () => {},
    }
  );

  assert.equal(result.kind, "discussion");
  if (result.kind === "discussion") {
    assert.ok(
      result.result.utterances.some((u) => u.act === "claim"),
      "dispatch 経由でも act 付き発話 (= dialectic 経路) になる"
    );
  }
  const d = db();
  const issues = d.prepare("SELECT COUNT(*) AS n FROM flow_issue WHERE session_id = ?").get("dlx-dispatch") as any;
  assert.ok(issues.n > 0, "dispatch 経由で flow_issue が立つ");
  d.close();

  delete process.env.DISCUTERE_FLOW_ENGINE;
  _resetConfig();
  console.log("  [ok] dispatch: flow.engine=dialectic で dialectic 経路に分岐");
}

// クリーンアップ
delete process.env.DISCUTERE_FLOW_PERSONA_COUNT;
delete process.env.DISCUTERE_FLOW_VOTER_COUNT;
delete process.env.DATABASE_PATH;
_resetConfig();
_resetFlowDb();

console.log("dialectic-driver tests: all passed");
