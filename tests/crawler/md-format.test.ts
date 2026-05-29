import assert from "node:assert/strict";

import {
  GameKGParseError,
  parseGameKG,
  stringifyGameKG,
} from "../../src/crawler/index.js";

const SAMPLE = `---
id: hollow-knight
title: "Hollow Knight"
genre: "メトロイドヴァニア"
workspace_id: "knowledge"
sources:
  - url: "https://example.com/hk"
    title: "HK guide"
    excerpt_policy: "summary-only"
mechanics:
  - name: "Dash"
    description: "短距離移動"
    intends: "回避と探索を兼ねる"
  - name: "Charm Equip"
aesthetics:
  - name: "孤独な探索"
---

# 概要

本文サンプル。
`;

// 1. parse の必須キー検証
{
  const kg = parseGameKG(SAMPLE);
  assert.equal(kg.id, "hollow-knight");
  assert.equal(kg.title, "Hollow Knight");
  assert.equal(kg.genre, "メトロイドヴァニア");
  assert.equal(kg.workspace_id, "knowledge");
  assert.equal(kg.sources.length, 1);
  assert.equal(kg.sources[0].url, "https://example.com/hk");
  assert.equal(kg.sources[0].excerpt_policy, "summary-only");
  assert.equal(kg.mechanics.length, 2);
  assert.equal(kg.mechanics[0].name, "Dash");
  assert.equal(kg.mechanics[0].intends, "回避と探索を兼ねる");
  assert.equal(kg.aesthetics.length, 1);
  assert.equal(kg.aesthetics[0].name, "孤独な探索");
  assert.ok(kg.body && kg.body.includes("本文サンプル"));
  console.log("ok parse minimum");
}

// 2. round-trip: stringify → parse で再構成されるか
{
  const kg = parseGameKG(SAMPLE);
  const md = stringifyGameKG(kg);
  const back = parseGameKG(md);
  assert.deepEqual(back.sources, kg.sources);
  assert.deepEqual(back.mechanics, kg.mechanics);
  assert.deepEqual(back.aesthetics, kg.aesthetics);
  assert.equal(back.title, kg.title);
  assert.equal(back.id, kg.id);
  console.log("ok round-trip");
}

// 3. id が kebab-case でない → エラー
{
  const bad = SAMPLE.replace("id: hollow-knight", "id: HollowKnight");
  assert.throws(() => parseGameKG(bad), GameKGParseError);
  console.log("ok reject non-kebab id");
}

// 4. sources[].url 必須
{
  const bad = `---
id: x
title: "X"
workspace_id: "knowledge"
sources:
  - title: "no url"
mechanics: []
aesthetics: []
---
`;
  assert.throws(() => parseGameKG(bad), GameKGParseError);
  console.log("ok reject source without url");
}

// 5. excerpt_policy の値検証
{
  const bad = SAMPLE.replace("summary-only", "anything-goes");
  assert.throws(() => parseGameKG(bad), GameKGParseError);
  console.log("ok reject unknown excerpt_policy");
}

// 6. workspace_id 未指定 → DEFAULT_WORKSPACE
{
  const noWs = `---
id: a
title: "A"
---
`;
  const kg = parseGameKG(noWs);
  assert.equal(kg.workspace_id, "knowledge");
  assert.equal(kg.sources.length, 0);
  assert.equal(kg.mechanics.length, 0);
  console.log("ok default workspace");
}

console.log("md-format.test.ts: all passed");
