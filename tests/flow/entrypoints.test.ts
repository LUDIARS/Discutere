/**
 * T7 起動経路 テスト。
 * - parseFlowKind: ラベル → FlowKind (議論/改善/学習/壁打ち)、未指定/不正は null
 * - dispatchFlow: フロー選択に応じて正しいドライバを起動 / flow 必須 (不正は受理しない)
 * - parseForumEntry / handleForumFlowPost: フォーラム適用タグ → (flow, tags)、flow なしは拒否
 * - Web ルート: テーマ + 議論タイプ + タグ で起動、状態ポーリング
 */

import assert from "node:assert/strict";
import path from "node:path";
import fs from "node:fs";

import { parseFlowKind, dispatchFlow, FlowTypeRequiredError } from "../../src/flow/dispatch.js";
import { parseForumEntry, handleForumFlowPost } from "../../src/flow/entry-discord.js";

// ── parseFlowKind ─────────────────────────────────────────────────────────────
{
  assert.equal(parseFlowKind("議論"), "discussion");
  assert.equal(parseFlowKind("discussion"), "discussion");
  assert.equal(parseFlowKind("改善"), "improvement");
  assert.equal(parseFlowKind("改善提案"), "improvement");
  assert.equal(parseFlowKind("学習"), "learning");
  assert.equal(parseFlowKind("壁打ち"), "sparring");
  assert.equal(parseFlowKind("sparring"), "sparring");
  assert.equal(parseFlowKind(""), null, "空は null");
  assert.equal(parseFlowKind(undefined), null, "undefined は null");
  assert.equal(parseFlowKind("雑談"), null, "未知ラベルは null");
  console.log("  [ok] parseFlowKind: ラベル → FlowKind / 未指定は null");
}

// ── dispatchFlow: フロー選択 → 正しいドライバ (注入スパイで検証) ─────────────
{
  const calls: string[] = [];
  const fakeDirectorResult = {
    sessionId: "s",
    paperId: "p",
    utterances: [],
    rounds: 1,
    conclusion: "x",
    concluded: true,
  };
  const fakeLearning = { gameSlug: "g", opinionsRecorded: 0, mechanicsRecorded: 0, polarityBreakdown: {} };
  const drivers = {
    discussion: async () => {
      calls.push("discussion");
      return fakeDirectorResult;
    },
    improvement: async () => {
      calls.push("improvement");
      return fakeDirectorResult;
    },
    learning: async () => {
      calls.push("learning");
      return fakeLearning;
    },
  };
  const baseDeps = { llm: { invoke: async () => ({ ok: true as const, text: "" }) }, core: {} as never, drivers };

  await dispatchFlow({ theme: "t", tags: [], flow: "議論" }, baseDeps);
  await dispatchFlow({ theme: "t", tags: [], flow: "改善" }, baseDeps);
  await dispatchFlow({ theme: "t", tags: [], flow: "学習" }, baseDeps);
  assert.deepEqual(calls, ["discussion", "improvement", "learning"], "ラベルごとに正しいドライバ");

  // 壁打ちは SparringSession を返す (start 済み) — LLM は呼ばれない (start は調査のみ)
  const spar = await dispatchFlow(
    { theme: "壁打ちテーマ", tags: [], flow: "壁打ち" },
    { llm: { invoke: async () => ({ ok: true as const, text: "" }) }, gamesDir: path.resolve(".tmp/none-xyz") }
  );
  assert.equal(spar.kind, "sparring", "壁打ち → sparring");
  if (spar.kind === "sparring") assert.ok(typeof spar.session.submitUser === "function", "SparringSession を返す");

  // flow 必須: 不正/未指定は受理しない
  await assert.rejects(
    () => dispatchFlow({ theme: "t", tags: [], flow: "" }, baseDeps),
    FlowTypeRequiredError,
    "flow 未指定は FlowTypeRequiredError"
  );
  await assert.rejects(() => dispatchFlow({ theme: "t", tags: [], flow: "雑談" }, baseDeps), FlowTypeRequiredError);
  console.log("  [ok] dispatchFlow: フロー選択 → 正しいドライバ / flow 必須");
}

// ── parseForumEntry: フォーラム適用タグ → (flow, tags) ──────────────────────
{
  assert.deepEqual(parseForumEntry(["改善", "機密"]), { flow: "improvement", tags: ["機密"] }, "改善 + 機密");
  assert.deepEqual(parseForumEntry(["壁打ち", "運用"]), { flow: "sparring", tags: ["運用"] }, "壁打ち + 運用");
  assert.deepEqual(parseForumEntry(["議論"]), { flow: "discussion", tags: [] }, "議論のみ");
  // 議論タイプタグが無い → flow null (受理しない方針)
  assert.deepEqual(parseForumEntry(["面白さ"]).flow, null, "旧方向タグのみは flow null");
  assert.deepEqual(parseForumEntry([]).flow, null, "タグなしは flow null");
  console.log("  [ok] parseForumEntry: 適用タグ → flow + tags");
}

// ── handleForumFlowPost: flow なしは拒否 / あれば dispatch ───────────────────
{
  const calls: string[] = [];
  const drivers = {
    improvement: async () => {
      calls.push("improvement");
      return { sessionId: "s", paperId: "p", utterances: [], rounds: 1, conclusion: "", concluded: false };
    },
  };
  const deps = { llm: { invoke: async () => ({ ok: true as const, text: "" }) }, drivers };

  const rejected = await handleForumFlowPost({ theme: "テーマ", appliedTagNames: ["面白さ"] }, deps);
  assert.equal(rejected.ok, false, "議論タイプタグなしは受理しない");
  if (!rejected.ok) assert.equal(rejected.reason, "flow-required");

  const accepted = await handleForumFlowPost({ theme: "テーマ", appliedTagNames: ["改善", "内部"] }, deps);
  assert.equal(accepted.ok, true, "改善タグで受理");
  assert.deepEqual(calls, ["improvement"], "improvement ドライバが呼ばれる");
  console.log("  [ok] handleForumFlowPost: flow 必須 / 適用タグから dispatch");
}

// ── Web ルート (Hono app.request) ───────────────────────────────────────────
{
  const TMP = path.resolve(".tmp/flow-entrypoints-web");
  fs.rmSync(TMP, { recursive: true, force: true });
  fs.mkdirSync(TMP, { recursive: true });
  process.env.DATABASE_PATH = path.join(TMP, "flow.db");

  const { _resetFlowDb } = await import("../../src/flow/db/connection.js");
  const { _resetConfig } = await import("../../src/config.js");
  const { MockLLMClient } = await import("../../src/persona-engine/llm/mock.js");
  const { Hono } = await import("hono");
  const { flowRoutes, setFlowWebDeps, _resetFlowWeb, normalizeSyntheticLearningOpinions } = await import(
    "../../src/flow/web/routes.js"
  );

  _resetFlowDb();
  _resetConfig();
  _resetFlowWeb();
  process.env.DISCUTERE_FLOW_PERSONA_COUNT = "2";
  process.env.DISCUTERE_FLOW_SPARRING_MAX_TURNS = "10";

  const mock = new MockLLMClient([], "AI 応答です");
  setFlowWebDeps({ workspaceId: "knowledge", llm: mock, gamesDir: path.join(TMP, "no-games") });

  const app = new Hono();
  app.route("/", flowRoutes);

  const post = (url: string, body: unknown) =>
    app.request(url, { method: "POST", headers: { "content-type": "application/json" }, body: JSON.stringify(body) });

  // 議論タイプ未指定 → 400
  const r400 = await post("/api/flow/start", { theme: "テーマ" });
  assert.equal(r400.status, 400, "flow 未指定は 400");

  // テーマ未指定 → 400
  const rEditGone = await post("/api/flow/nope/paper/edit", { instruction: "全体を調整して" });
  assert.equal(rEditGone.status, 410, "旧 全体調整 API は廃止");
  const editGoneBody = (await rEditGone.json()) as { ok: boolean; error: string };
  assert.equal(editGoneBody.ok, false, "旧 全体調整 API は LLM に流さない");

  const rNoTheme = await post("/api/flow/start", { theme: "", flow: "discussion" });
  assert.equal(rNoTheme.status, 400, "テーマ未指定は 400");

  // 壁打ち起動 → sessionId、say → 応答、status → 発話取得
  const rStart = await post("/api/flow/start", { theme: "壁打ちテーマ", flow: "壁打ち", tags: [] });
  const startBody = (await rStart.json()) as { ok: boolean; kind: string; sessionId: string };
  assert.equal(startBody.ok, true, "壁打ち start 成功");
  assert.equal(startBody.kind, "sparring");
  assert.ok(startBody.sessionId, "sessionId 返却");

  const rSay = await post(`/api/flow/${startBody.sessionId}/say`, { text: "最初の意見" });
  assert.equal(rSay.status, 200, "say 成功");

  const rStatus = await app.request(`/api/flow/${startBody.sessionId}/status?since=0`);
  const statusBody = (await rStatus.json()) as { ok: boolean; utterances: Array<{ role: string }> };
  assert.equal(statusBody.ok, true, "status 取得");
  assert.ok(statusBody.utterances.length >= 1, "ユーザ + AI 発話が status に出る");
  assert.ok(statusBody.utterances.some((u) => u.role === "user"), "ユーザ発話含む");

  // say 先が存在しない session → 404
  const r404 = await post("/api/flow/nope/say", { text: "x" });
  assert.equal(r404.status, 404, "未知 session の say は 404");

  // GET /flow は HTML
  const rPage = await app.request("/flow");
  assert.equal(rPage.status, 200, "/flow は 200");
  const pageHtml = await rPage.text();
  assert.ok(pageHtml.includes("議論タイプ"), "UI に議論タイプ選択がある");
  assert.ok(pageHtml.includes("ゲームタイトル（または主目的）"), "UI にゲームタイトル欄がある");
  assert.ok(pageHtml.includes("議論したいテーマ"), "UI に議論テーマ欄がある");
  assert.ok(pageHtml.includes("システム/メカニクスの説明"), "UI にシステム/メカニクス欄がある");
  assert.ok(pageHtml.includes("議論一覧"), "UI に議論一覧がある");
  assert.ok(pageHtml.includes("新規議論開始"), "UI に新規議論開始ボタンがある");
  assert.ok(pageHtml.includes('data-state="draft"'), "UI に state フィルタタブがある");
  assert.ok(pageHtml.includes("もっと見る"), "UI に「もっと見る」(ページング) がある");
  assert.ok(pageHtml.includes('id="rvCheckDebate"'), "UI に 議論可能か確認する ボタンがある");
  assert.ok(pageHtml.includes('id="rvRefreshSuggestions"'), "UI に 指摘内容の修正提案 ボタンがある");
  assert.ok(pageHtml.includes('id="rvMechanicsCheck"'), "UI に メカニクス知識確認 ボタンがある");
  assert.ok(pageHtml.includes('id="rvLearningLink"'), "UI に 追加学習ページ導線がある");
  assert.ok(pageHtml.includes('id="voiceSimulation"'), "UI にユーザの声シミュレーション示唆がある");
  assert.ok(pageHtml.includes("ユーザの声のLLM生成シミュレーション"), "UI に LLM 生成シミュレーション可否の表示がある");
  assert.ok(pageHtml.includes("apply-fix"), "UI に修正提案の調整ボタン処理がある");
  assert.ok(pageHtml.includes("/fix-suggestion/apply"), "UI に修正提案適用 API 呼び出しがある");
  assert.ok(!pageHtml.includes('id="rvEdit"'), "旧 全体調整 入力は表示しない");
  assert.ok(!pageHtml.includes('id="rvEditBtn"'), "旧 全体調整 ボタンは表示しない");
  const pageScript = /<script>([\s\S]*?)<\/script>/.exec(pageHtml)?.[1];
  assert.ok(pageScript, "UI に script がある");
  assert.doesNotThrow(() => new Function(pageScript), "UI script が構文エラーなく parse できる");

  const syntheticVoices = normalizeSyntheticLearningOpinions(
    [
      {
        segment: "初心者",
        polarity: "negative",
        concern: "チュートリアル理解",
        content: "[synthetic] 何をすれば強くなるのか最初に分からず、説明を飛ばしたら戻れないのがつらい。",
      },
      {
        segment: "初心者",
        polarity: "negative",
        concern: "チュートリアル理解",
        content: "[synthetic] 最初に何をすれば強くなるのか分からず、説明を飛ばすと戻れないのがつらい。",
      },
      {
        segment: "復帰者",
        polarity: "mixed",
        concern: "UI変化",
        content: "[synthetic] 昔より便利そうだが、復帰直後は画面の情報量が多くて迷う。",
      },
      {
        segment: "上級者",
        polarity: "positive",
        concern: "序盤テンポ",
        content: "[synthetic] 既存プレイヤーには短い導線だけで十分で、すぐクエストへ行ける方が良い。",
      },
      {
        segment: "無課金",
        polarity: "negative",
        concern: "ガチャ導線",
        content: "[synthetic] 序盤からガチャと育成素材の説明が絡むと、課金前提に見えて身構える。",
      },
      {
        segment: "友達紹介",
        polarity: "mixed",
        concern: "協力プレイ",
        content: "[synthetic] 友達に誘われた人にはマルチの楽しさを早く見せたいが、操作説明が長いと離れそう。",
      },
    ],
    1000
  );
  assert.equal(syntheticVoices.length, 5, "仮想ユーザ声は同じセグメント+論点の重複を除外する");
  assert.ok(syntheticVoices.every((v) => v.content.startsWith("[synthetic] (")), "仮想声は仮説ラベルと軸を保持する");
  assert.ok(syntheticVoices.some((v) => v.content.includes("復帰者")), "異なるユーザ層は残す");

  // 議論一覧: 開始済み (discussion_paper 永続) の議論が在庫として並ぶ (進行中も含む)。
  const { persistPaper, persistDraftPaper, upsertDiscussionTitleCache, setPaperReviewInfo } = await import(
    "../../src/flow/discussion-paper.js"
  );
  const { paperDraftToMarkdown } = await import("../../src/flow/paper-markdown.js");
  const { getFlowDb } = await import("../../src/flow/db/connection.js");
  persistPaper(
    { sessionId: "list-sess-1", theme: "一覧テスト議題", tags: [], mechanics: [], supplement: "", bodyMd: "# 議題\n一覧テスト議題" },
    "discussion"
  );
  persistPaper(
    { sessionId: "done-sess-1", theme: "収束前の議題", tags: [], mechanics: [], supplement: "", bodyMd: "# 議題\n収束前の議題" },
    "discussion"
  );
  getFlowDb()
    .prepare(
      "INSERT INTO flow_conclusion (session_id, paper_id, summary, aufhebung_json, top_utterance_ids_json, concluded, created_at) VALUES (?, ?, ?, ?, ?, ?, ?)"
    )
    .run("done-sess-1", "done-sess-1", "【収束】収束時のタイトル\n詳細本文", "[]", "[]", 1, Date.now());
  upsertDiscussionTitleCache("done-sess-1", "【収束】収束時のタイトル\n詳細本文", "conclusion");
  // ドラフト (未確定) も一覧に「下書き」として出る + 編集を再開できる。
  persistDraftPaper(
    { sessionId: "draft-sess-1", theme: "下書き議題", tags: [], mechanics: [], supplement: "", bodyMd: "# 議題\n下書き議題" },
    "discussion"
  );
  setPaperReviewInfo("draft-sess-1", {
    voiceCount: 0,
    countCapped: false,
    samples: [],
    fixSuggestions: [
      {
        title: "論点補強",
        reason: "対立軸が弱い",
        suggestedChange: "{{具体例}} をもとに、賛成側と反対側の争点を追記する。",
      },
    ],
  });
  const rSessions = await app.request("/api/flow/sessions");
  const sessionsBody = (await rSessions.json()) as {
    ok: boolean;
    sessions: Array<{
      sessionId: string;
      title: string;
      theme: string;
      concluded: boolean;
      state: string;
      discussionType: string;
      originUi: string;
    }>;
  };
  assert.equal(sessionsBody.ok, true, "sessions 取得");
  const listed = sessionsBody.sessions.find((s) => s.sessionId === "list-sess-1");
  assert.ok(listed, "開始済み議論が一覧に出る (一度投げたら保存=在庫表示)");
  assert.equal(listed!.theme, "一覧テスト議題");
  assert.equal(listed!.title, "一覧テスト議題", "未収束はディスカッションペーパー議題を表示タイトルにする");
  assert.equal(listed!.concluded, false, "未収束は concluded=false");
  assert.equal(listed!.state, "live", "開始済み未収束は state=live");
  const done = sessionsBody.sessions.find((s) => s.sessionId === "done-sess-1");
  assert.ok(done, "収束済み議論が一覧に出る");
  assert.equal(done!.title, "収束時のタイトル", "収束済みは収束時のタイトルを表示タイトルにする");
  assert.equal(done!.discussionType, "discussion", "一覧キャッシュは議論タイプを返す");
  assert.equal(done!.originUi, "ai", "AI議論で作った議論は originUi=ai");
  const draft = sessionsBody.sessions.find((s) => s.sessionId === "draft-sess-1");
  assert.ok(draft, "ドラフトも一覧に出る");
  assert.equal(draft!.state, "draft", "ドラフトは state=draft");
  // ドラフトは /paper で復元でき編集を再開できる (メモリに無くても rehydrate)。
  const rDraftPaper = await app.request("/api/flow/draft-sess-1/paper");
  const draftPaper = (await rDraftPaper.json()) as { ok: boolean; ready: boolean; paper: { bodyMd: string } | null };
  assert.equal(draftPaper.ok, true, "draft paper 取得");
  assert.equal(draftPaper.ready, true, "draft は ready (rehydrate)");
  assert.ok(draftPaper.paper?.bodyMd.includes("下書き議題"), "復元した本文が読める");
  const rApplyFix = await post("/api/flow/draft-sess-1/paper/fix-suggestion/apply", {
    suggestionIndex: 0,
    text: "周回報酬の緩和案をもとに、賛成側と反対側の争点を追記する。",
  });
  assert.equal(rApplyFix.status, 200, "修正提案を適用できる");
  const appliedFix = (await rApplyFix.json()) as {
    ok: boolean;
    fixedFields: { discussionContent: string };
    info: {
      fixSuggestions: Array<{ appliedAt?: number; appliedText?: string }>;
      voiceSimulation?: { possible: boolean; confidence: string; summary: string };
    };
  };
  assert.equal(appliedFix.ok, true, "修正提案適用 ok");
  assert.ok(appliedFix.fixedFields.discussionContent.includes("修正追記: 論点補強"), "議論内容へ追記される");
  assert.ok(appliedFix.fixedFields.discussionContent.includes("周回報酬の緩和案"), "入力済みテキストが追記される");
  assert.ok(appliedFix.info.fixSuggestions[0].appliedAt, "提案は反映済みとして保存される");
  assert.ok(appliedFix.info.voiceSimulation?.summary, "ユーザの声の LLM 生成シミュレーション可否を返す");
  const rStartedPaper = await app.request("/api/flow/list-sess-1/paper");
  const startedPaper = (await rStartedPaper.json()) as { ok: boolean; ready: boolean; started?: boolean; status?: string };
  assert.equal(rStartedPaper.status, 200, "開始済み paper 取得も 404 にしない");
  assert.equal(startedPaper.ok, true, "開始済み paper 取得");
  assert.equal(startedPaper.started, true, "開始済み paper は started=true");
  assert.equal(startedPaper.status, "started", "開始済み paper の status を返す");

  const simBody = paperDraftToMarkdown({
    theme: "仮想ユーザ補填テーマ",
    gameTitle: "Test Game",
    discussionTheme: "仮想ユーザ補填テーマ",
    discussionContent: "既存の実ユーザ声は肯定に偏っている。",
    mechanicsContext: "基本ループはステージ周回、報酬獲得、強化、再挑戦で構成される。失敗時の損失、報酬密度、待ち時間、課金圧がユーザ反応を左右する。",
    themeSupplement: "",
    tags: [],
    supplement: "",
    mechanics: [{ name: "周回", description: "報酬獲得と強化を繰り返す" }],
  });
  persistDraftPaper(
    { sessionId: "sim-sess-1", theme: "仮想ユーザ補填テーマ", tags: [], mechanics: [{ name: "周回", description: "報酬獲得と強化を繰り返す" }], supplement: "", bodyMd: simBody },
    "discussion"
  );
  setPaperReviewInfo("sim-sess-1", {
    voiceCount: 50,
    countCapped: true,
    samples: [],
    debatability: {
      issues: ["争点A", "争点B", "争点C"],
      armability: [
        { issue: "争点A", armable: "pro-only" },
        { issue: "争点B", armable: "pro-only" },
        { issue: "争点C", armable: "neither" },
      ],
      armableBothCount: 0,
      minArmableIssues: 2,
      evidence: { voiceCount: 50, positive: 50, negative: 0, neutral: 0, polaritySkew: 1, dominantSource: { name: "learning", share: 1 } },
      debatable: false,
      degraded: false,
      recommendation: { flow: "learning", reason: "材料不足" },
      message: "議論適性: 低",
    },
  });
  const rSimPaper = await app.request("/api/flow/sim-sess-1/paper");
  const simPaper = (await rSimPaper.json()) as {
    ok: boolean;
    info: {
      debatability: { debatable: boolean; recommendation: unknown; message: string };
      voiceSimulation: { possible: boolean; confidence: string };
    };
  };
  assert.equal(simPaper.ok, true, "simulation paper 取得");
  assert.equal(simPaper.info.voiceSimulation.confidence, "high", "ユーザの声を高信頼で仮説補填できる");
  assert.equal(simPaper.info.debatability.debatable, true, "LLM 仮想ユーザ補填可能なら議論適性あり");
  assert.equal(simPaper.info.debatability.recommendation, null, "学習再提案は消える");
  assert.ok(simPaper.info.debatability.message.includes("仮想ユーザ補填"), "昇格理由を message に残す");

  // 絞り込み: state=draft はドラフトのみ、state=live は開始済み未収束のみ。
  const rDraftOnly = (await (await app.request("/api/flow/sessions?state=draft")).json()) as {
    sessions: Array<{ sessionId: string; state: string }>;
    total: number;
  };
  assert.ok(rDraftOnly.sessions.every((s) => s.state === "draft"), "state=draft は draft のみ");
  assert.ok(rDraftOnly.sessions.some((s) => s.sessionId === "draft-sess-1"), "draft 絞り込みに draft-sess-1");
  assert.ok(!rDraftOnly.sessions.some((s) => s.sessionId === "list-sess-1"), "draft 絞り込みに live は出ない");
  const rLiveOnly = (await (await app.request("/api/flow/sessions?state=live")).json()) as {
    sessions: Array<{ sessionId: string; state: string }>;
  };
  assert.ok(rLiveOnly.sessions.some((s) => s.sessionId === "list-sess-1"), "live 絞り込みに list-sess-1");
  assert.ok(!rLiveOnly.sessions.some((s) => s.sessionId === "draft-sess-1"), "live 絞り込みに draft は出ない");

  // 共通一覧 scope: all は全件、ai は AI 議論のみ、chat はチャット/壁打ちのみ。
  const rAiScope = (await (await app.request("/api/flow/sessions?scope=ai")).json()) as {
    sessions: Array<{ sessionId: string; originUi: string; discussionType: string }>;
  };
  assert.ok(rAiScope.sessions.some((s) => s.sessionId === "list-sess-1"), "scope=ai に AI 議論が出る");
  assert.ok(!rAiScope.sessions.some((s) => s.sessionId === startBody.sessionId), "scope=ai に壁打ちは出ない");
  const rChatScope = (await (await app.request("/api/flow/sessions?scope=chat")).json()) as {
    sessions: Array<{ sessionId: string; originUi: string; discussionType: string }>;
  };
  assert.ok(rChatScope.sessions.some((s) => s.sessionId === startBody.sessionId), "scope=chat に壁打ちが出る");
  assert.ok(rChatScope.sessions.every((s) => s.originUi === "chat" || s.discussionType === "sparring"), "scope=chat はチャット/壁打ちのみ");

  // status: 投票集計はマークとして返し、止揚は世論として使われた意見だけに付く。
  const markPaperId = persistPaper(
    { sessionId: "mark-sess-1", theme: "マーク議題", tags: [], mechanics: [], supplement: "", bodyMd: "# 議題\nマーク議題" },
    "discussion"
  );
  const markDb = getFlowDb();
  markDb
    .prepare(
      `INSERT INTO flow_utterance
         (id, session_id, paper_id, round, turn, persona_id, persona_name, role, stance, text, is_error, created_at)
       VALUES
         ('u-voted', 'mark-sess-1', ?, 1, 1, 'p1', '論者A', 'opinion', 'opinion', '意見A', 0, 100),
         ('u-other', 'mark-sess-1', ?, 1, 2, 'p2', '論者B', 'debater', 'pro', '意見B', 0, 101)`
    )
    .run(markPaperId, markPaperId);
  markDb
    .prepare("INSERT INTO vote (session_id, round, voter_index, chosen_utterance_id, created_at) VALUES (?, ?, ?, ?, ?)")
    .run("mark-sess-1", 1, 0, "u-voted", 200);
  markDb
    .prepare("INSERT INTO vote (session_id, round, voter_index, chosen_utterance_id, created_at) VALUES (?, ?, ?, ?, ?)")
    .run("mark-sess-1", 1, 1, "u-voted", 201);
  markDb
    .prepare("INSERT INTO discussion_paper_round (paper_id, round, summary, aufhebung_json, created_at) VALUES (?, ?, ?, ?, ?)")
    .run(markPaperId, 1, "まとめ", JSON.stringify(["止揚案"]), 202);
  const markStatus = (await (await app.request("/api/flow/mark-sess-1/status?since=0")).json()) as {
    utterances: Array<{ id: string; votes: number; isWinner: boolean; roundAufhebung: string[] }>;
    marks: Array<{ id: string; votes: number; isWinner: boolean; roundAufhebung: string[] }>;
  };
  const voted = markStatus.utterances.find((u) => u.id === "u-voted");
  const other = markStatus.utterances.find((u) => u.id === "u-other");
  assert.equal(voted?.votes, 2, "投票をもらった意見に集計が付く");
  assert.equal(voted?.isWinner, true, "最多得票意見は採択扱い");
  assert.deepEqual(voted?.roundAufhebung, ["止揚案"], "止揚は使われた意見だけに付く");
  assert.deepEqual(other?.roundAufhebung, [], "同ラウンドの他意見には止揚を付けない");
  assert.ok(markStatus.marks.some((m) => m.id === "u-voted" && m.votes === 2), "差分ポーリング用 marks も返る");

  // ページング: limit=1 で 1 件 + hasMore、offset でずらすと別件。
  const rPage1 = (await (await app.request("/api/flow/sessions?limit=1&offset=0")).json()) as {
    sessions: Array<{ sessionId: string }>;
    total: number;
    hasMore: boolean;
  };
  assert.equal(rPage1.sessions.length, 1, "limit=1 は 1 件");
  assert.ok(rPage1.total >= 2, "total は全件数 (>=2)");
  assert.equal(rPage1.hasMore, true, "残りがあるので hasMore=true");
  const rPage2 = (await (await app.request("/api/flow/sessions?limit=1&offset=1")).json()) as {
    sessions: Array<{ sessionId: string }>;
  };
  assert.notEqual(rPage1.sessions[0].sessionId, rPage2.sessions[0].sessionId, "offset で別件を返す");

  // 削除: 派生行も含めて消える + 一覧から除かれる。未知 session は 404。
  const { deleteFlowSession } = await import("../../src/flow/discussion-paper.js");
  const rDel = await app.request("/api/flow/draft-sess-1", { method: "DELETE" });
  assert.equal(rDel.status, 200, "ドラフト削除は 200");
  const afterDel = (await (await app.request("/api/flow/sessions")).json()) as {
    sessions: Array<{ sessionId: string }>;
  };
  assert.ok(!afterDel.sessions.some((s) => s.sessionId === "draft-sess-1"), "削除後は一覧から消える");
  assert.ok(afterDel.sessions.some((s) => s.sessionId === "list-sess-1"), "他の議論は残る");
  const rDel404 = await app.request("/api/flow/nope-session", { method: "DELETE" });
  assert.equal(rDel404.status, 404, "未知 session の削除は 404");
  // データ層: 派生行 (発話/版履歴) も session ごと消える。
  const fdb = getFlowDb();
  fdb.prepare(
    `INSERT INTO flow_utterance (id, session_id, paper_id, round, turn, persona_id, persona_name, role, text, created_at)
     VALUES ('u-del-1', 'list-sess-1', 'p', 1, 1, 'pi', 'pn', 'opinion', 't', 1)`
  ).run();
  assert.equal(deleteFlowSession("list-sess-1"), true, "deleteFlowSession は対象ありで true");
  const left = fdb.prepare(`SELECT COUNT(*) AS n FROM flow_utterance WHERE session_id = ?`).get("list-sess-1") as { n: number };
  assert.equal(left.n, 0, "派生発話も消える");
  assert.equal(deleteFlowSession("list-sess-1"), false, "二重削除は false (対象なし)");

  console.log("  [ok] flow web routes: start / 壁打ち / status / 404 / UI / 議論一覧 + ドラフト + 削除");

  delete process.env.DISCUTERE_FLOW_PERSONA_COUNT;
  delete process.env.DISCUTERE_FLOW_SPARRING_MAX_TURNS;
  delete process.env.DATABASE_PATH;
  _resetFlowDb();
}

console.log("entrypoints (T7) tests: all passed");
