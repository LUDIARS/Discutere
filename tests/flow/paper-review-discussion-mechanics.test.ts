/**
 * 議論 (flow=discussion) のペーパーに、ゲーム名で一致した data/games のメカニクスが載ることのテスト。
 *   - ゲーム名 (seed.gameTitle、無ければ議題) に一致する md の frontmatter mechanics を読む
 *   - 一致しなければ空 (企画段階の新作)
 *   - 外部収集 (youtubeSearch) と LLM 増補 (enrichMechanics) はしない
 */

import assert from "node:assert/strict";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";

import { buildPaperDraft } from "../../src/flow/paper-review.js";
import { paperDraftToMarkdown } from "../../src/flow/paper-markdown.js";
import { MockLLMClient } from "../../src/persona-engine/llm/mock.js";

const gamesDir = fs.mkdtempSync(path.join(os.tmpdir(), "discutere-discussion-mechanics-"));
fs.writeFileSync(
  path.join(gamesDir, "monster-strike.md"),
  [
    "---",
    "id: monster-strike",
    "title: モンスターストライク",
    "mechanics:",
    "  - name: ギミック",
    "    description: クエストごとに重力バリアやダメージウォールなどの妨害が置かれる",
    "  - name: アビリティ",
    "    description: アンチ重力バリア等でギミックに対応し、編成を決める",
    "---",
    "",
    "本文",
    "",
  ].join("\n"),
  "utf-8"
);
fs.writeFileSync(
  path.join(gamesDir, "other-game.md"),
  ["---", "id: other-game", "title: 別のゲーム", "mechanics:", "  - name: 無関係", "    description: 混ざってはいけない", "---", ""].join("\n"),
  "utf-8"
);

/** 増補が呼ばれたら source=llm のメカニクスを返す LLM (呼ばれないことを確認する)。 */
function enrichSpy(): MockLLMClient {
  return new MockLLMClient([
    () => '{"mechanics":[{"name":"LLM増補","description":"仮説"}]}',
  ]);
}

const seedFor = (gameTitle: string) => ({
  gameTitle,
  discussionTheme: "長寿コンテンツとして遊ばれ続ける理由",
  discussionContent: "",
  mechanicsContext: "",
  themeSupplement: "",
});

try {
  // ゲーム名が一致 → そのゲームのメカニクスだけが curated で載り、本文 md にも出る。
  {
    let searches = 0;
    const llm = enrichSpy();
    const seed = seedFor("モンスターストライク");
    const { draft } = await buildPaperDraft(seed.discussionTheme, [], {
      flow: "discussion",
      seed,
      gamesDir,
      llm,
      understandingLlm: new MockLLMClient([], '{"ok":true}'),
      youtubeSearch: async () => { searches++; return []; },
    });
    assert.deepEqual(draft.mechanics.map((m) => m.name), ["ギミック", "アビリティ"]);
    assert.ok(draft.mechanics.every((m) => m.source === "curated"), "出所は curated");
    assert.equal(searches, 0, "議論では YouTube 収集をしない");
    assert.equal(llm.calls, 0, "議論では LLM 増補をしない");
    const md = paperDraftToMarkdown(draft);
    assert.ok(md.includes("**ギミック**"), "本文 md の確認済みメカニクスに載る");
    assert.ok(md.includes("**アビリティ**"));
    assert.ok(!md.includes("無関係"), "一致しないゲームのメカニクスは混ざらない");
  }

  // ゲーム名が無ければ議題で照合する (seed なしでも議題がゲーム名なら載る)。
  {
    const { draft } = await buildPaperDraft("モンスターストライク", [], { flow: "discussion", gamesDir });
    assert.deepEqual(draft.mechanics.map((m) => m.name), ["ギミック", "アビリティ"]);
  }

  // 一致しないゲーム名 → 空のまま (従来どおり)。
  {
    const llm = enrichSpy();
    const seed = seedFor("存在しない新作");
    const { draft } = await buildPaperDraft(seed.discussionTheme, [], {
      flow: "discussion",
      seed,
      gamesDir,
      llm,
      understandingLlm: new MockLLMClient([], '{"ok":true}'),
    });
    assert.deepEqual(draft.mechanics, []);
    assert.equal(llm.calls, 0);
  }

  // 事前情報 (extraMechanics) とは名前で重複排除し、事前情報が先勝ち。
  {
    const seed = seedFor("モンスターストライク");
    const { draft } = await buildPaperDraft(seed.discussionTheme, [], {
      flow: "discussion",
      seed,
      gamesDir,
      extraMechanics: [{ name: "ギミック", description: "使い回し由来", source: "curated" }],
    });
    assert.deepEqual(draft.mechanics.map((m) => m.name), ["ギミック", "アビリティ"]);
    assert.equal(draft.mechanics[0]?.description, "使い回し由来");
  }
} finally {
  fs.rmSync(gamesDir, { recursive: true, force: true });
}

console.log("paper-review-discussion-mechanics: ok");
