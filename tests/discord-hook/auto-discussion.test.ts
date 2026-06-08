import fs from "node:fs";
import path from "node:path";
import assert from "node:assert/strict";

import { createCore } from "../../src/core/index.js";
import {
  classifyDiscordMessage,
  startAutoDiscussionForDiscordMessage,
} from "../../src/discord-hook/auto-discussion.js";
import { MockLLMClient } from "../../src/persona-engine/index.js";

const dir = path.resolve(".tmp/discord-auto-discussion");
fs.rmSync(dir, { recursive: true, force: true });
fs.mkdirSync(dir, { recursive: true });

const core = createCore(path.join(dir, "db.kuzu"), path.join(dir, "events.jsonl"));
const workspaceId = "knowledge";
const sessionId = core.repos.session.create({
  workspaceId,
  title: "discord-session:g1:ch1",
  startedAt: Date.now(),
  scene: "discord:g1/ch1",
});
const utteranceId = core.repos.utterance.create({
  workspaceId,
  sessionId,
  speakerId: "u1",
  rawContent: "ヴァンサバに必要な要素とは何か？",
  postedAt: Date.now(),
});

{
  const result = await startAutoDiscussionForDiscordMessage(core, {
    workspaceId,
    guildId: "g1",
    channelId: "ch1",
    sessionId,
    utteranceId,
    authorId: "u1",
    content: "ヴァンサバに必要な要素とは何か？",
  });
  assert.equal(result.started, true);
  assert.ok(result.gapId);

  const gap = core.client.raw
    .prepare("SELECT title, evidence_json FROM design_gaps WHERE id = ?")
    .get(result.gapId) as { title: string; evidence_json: string };
  assert.ok(gap.title.includes("Discord議題"));
  assert.ok(JSON.parse(gap.evidence_json).utteranceIds.includes(utteranceId));

  const event = core.client.raw
    .prepare("SELECT event_type FROM events WHERE event_type = 'DesignGapDetected' ORDER BY created_at DESC LIMIT 1")
    .get() as { event_type: string } | undefined;
  assert.equal(event?.event_type, "DesignGapDetected");
  console.log("ok auto discussion starts from fallback classification");
}

{
  const before = core.client.raw.prepare("SELECT COUNT(*) AS n FROM design_gaps").get() as { n: number };
  const result = await startAutoDiscussionForDiscordMessage(core, {
    workspaceId,
    guildId: "g1",
    channelId: "ch1",
    sessionId,
    utteranceId,
    authorId: "u1",
    content: "ヴァンサバに必要な要素とは何か？",
  });
  const after = core.client.raw.prepare("SELECT COUNT(*) AS n FROM design_gaps").get() as { n: number };
  assert.equal(result.started, false);
  assert.equal(after.n, before.n);
  console.log("ok auto discussion does not duplicate by utterance evidence");
}

{
  // gameTitle + genre が特定できれば start_discussion (LLM 経路が効いている)。
  const llm = new MockLLMClient([
    () =>
      JSON.stringify({
        action: "start_discussion",
        category: "opinion",
        gameTitle: "Vampire Survivors",
        genre: "ローグライト",
        title: "LLM分類議題",
        description: "LLMで分類した議題",
        reason: "mock",
      }),
  ]);
  const classification = await classifyDiscordMessage("Vampire Survivors はテンポが悪いと思う", llm);
  assert.equal(llm.calls, 1);
  assert.equal(classification.action, "start_discussion");
  assert.equal(classification.title, "LLM分類議題");
  assert.equal(classification.gameTitle, "Vampire Survivors");
  assert.equal(classification.genre, "ローグライト");
  console.log("ok auto discussion can use LLM classification (gameTitle/genre extracted)");
}

{
  // 対象ゲームを特定できない LLM start_discussion は record_only に降格 (判断ゲート)。
  const llm = new MockLLMClient([
    () =>
      JSON.stringify({
        action: "start_discussion",
        category: "opinion",
        gameTitle: "",
        title: "曖昧な議題",
        description: "どのゲームか不明",
        reason: "mock",
      }),
  ]);
  const classification = await classifyDiscordMessage("なんか最近のゲームつまらないよね", llm);
  assert.equal(classification.action, "record_only", "gameTitle 不明なら開始しない");
  console.log("ok classifier gates start when gameTitle is unidentified");
}

{
  // フォーラムタイトル (主題) を渡すと、 LLM が別タイトルを返しても gap タイトルは
  // フォーラムタイトルに固定され、 description にも主題が刻まれる (主題ずれ防止)。
  const llm = new MockLLMClient([
    () =>
      JSON.stringify({
        action: "start_discussion",
        category: "game_design_question",
        title: "全然違うゲームの議題",
        description: "本文だけから推測した説明",
        reason: "mock",
      }),
  ]);
  const result = await startAutoDiscussionForDiscordMessage(
    core,
    {
      workspaceId: "knowledge",
      guildId: "g1",
      channelId: "ch-forum",
      sessionId,
      utteranceId: `utt-forumtitle-${Date.now()}`,
      authorId: "u1",
      content: "目立ったアクションもないし、よじ登りもだるい。名作の要素はどこ？",
      forumTitle: "ワンダと巨像",
    },
    llm
  );
  assert.equal(result.started, true);
  const gap = core.client.raw
    .prepare("SELECT title, description FROM design_gaps WHERE id = ?")
    .get(result.gapId) as { title: string; description: string };
  assert.equal(gap.title, "ワンダと巨像", "gap タイトルはフォーラムタイトルに固定");
  assert.ok(gap.description.includes("ワンダと巨像"), "description にも主題が刻まれる");
  console.log("ok forumTitle anchors gap topic");
}

{
  // 同じ category + 同じ affect の議論を 2 件起こしても dedup ユニーク索引
  // (workspace_id, gap_in, expected_affect, observed_affect) で衝突しないこと。
  // (旧実装は gap_in=`discord:<category>` 固定で 2 件目が UNIQUE 制約違反で throw した。)
  const mk = (tag: string) => {
    const sid = core.repos.session.create({
      workspaceId,
      title: `discord-session:g1:${tag}`,
      startedAt: Date.now(),
      scene: `discord:g1/${tag}`,
    });
    const uid = core.repos.utterance.create({
      workspaceId,
      sessionId: sid,
      speakerId: "u1",
      rawContent: "このゲームのテンポは良いか悪いか議論したい",
      postedAt: Date.now(),
    });
    return { sid, uid };
  };
  const a = mk("th-a");
  const b = mk("th-b");
  const common = {
    workspaceId,
    guildId: "g1",
    authorId: "u1",
    content: "このゲームのテンポは良いか悪いか議論したい",
  };
  const ra = await startAutoDiscussionForDiscordMessage(core, {
    ...common, channelId: "th-a", sessionId: a.sid, utteranceId: a.uid,
  });
  const rb = await startAutoDiscussionForDiscordMessage(core, {
    ...common, channelId: "th-b", sessionId: b.sid, utteranceId: b.uid,
  });
  assert.equal(ra.started, true);
  assert.equal(rb.started, true, "2件目も UNIQUE 制約で落ちず議論が立つ");
  assert.notEqual(ra.gapId, rb.gapId);
  const rows = core.client.raw
    .prepare("SELECT gap_in FROM design_gaps WHERE id IN (?, ?)")
    .all(ra.gapId, rb.gapId) as Array<{ gap_in: string }>;
  assert.equal(new Set(rows.map((r) => r.gap_in)).size, 2, "gap_in が議論ごとに一意");
  console.log("ok 同カテゴリの議論が複数立っても dedup 衝突しない");
}

core.close();
console.log("auto-discussion.test.ts: all passed");
