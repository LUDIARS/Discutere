/**
 * Anatomia CLI クライアント (client.ts) のテスト。
 * 実 CLI は spawn せず AnatomiaCliRunner を注入して検証する。
 *   - domains list --json をパースする。
 *   - 空のとき autoDraft で draft → 再 list する。
 *   - exit!=0 / bin 不在 / JSON 不正は throw (fail-fast)。
 *   - source は project / repoPath を CLI 引数に変換する。
 */

import assert from "node:assert/strict";
import { fileURLToPath } from "node:url";

import { fetchAnatomiaDomains, type AnatomiaCliRunner } from "../../src/flow/anatomia/client.js";

// existsSync を通すため、確実に存在するファイルを binPath に使う (このテストファイル自身)。
const EXISTING_BIN = fileURLToPath(import.meta.url);

const RESPONSE = JSON.stringify({
  dir: "X/.anatomia/domains",
  domains: [
    { name: "Trial", description: "裁判", mechanics: ["trial"], specRefs: ["spec/A"], rationale: "境界" },
    { name: "NoName placeholder", description: "x", mechanics: [] },
    { description: "name 無しは落ちる", mechanics: [] },
  ],
});

/** 呼び出しを記録し、list 引数に応じて応答を返す runner を作る。 */
function makeRunner(listOutputs: string[]): { runner: AnatomiaCliRunner; calls: string[][] } {
  const calls: string[][] = [];
  let listIdx = 0;
  const runner: AnatomiaCliRunner = {
    async run(_bin, args) {
      calls.push(args);
      if (args[1] === "list") {
        const out = listOutputs[Math.min(listIdx, listOutputs.length - 1)];
        listIdx += 1;
        return { stdout: out, exitCode: 0 };
      }
      return { stdout: "", exitCode: 0 }; // draft
    },
  };
  return { runner, calls };
}

// ── 正常: list をパース (name 無しは落ちる) ─────────────────────────────────
{
  const { runner, calls } = makeRunner([RESPONSE]);
  const domains = await fetchAnatomiaDomains({ project: "pagus" }, { binPath: EXISTING_BIN, runner });
  assert.equal(domains.length, 2, "name を持つ 2 件のみ");
  assert.equal(domains[0].name, "Trial");
  assert.deepEqual(domains[0].mechanics, ["trial"]);
  assert.deepEqual(domains[0].specRefs, ["spec/A"]);
  assert.deepEqual(calls[0], ["domains", "list", "--project", "pagus", "--json"]);
}

// ── repoPath ソース → --repo 引数 ───────────────────────────────────────────
{
  const { runner, calls } = makeRunner([RESPONSE]);
  await fetchAnatomiaDomains({ repoPath: "E:/x/Pagus" }, { binPath: EXISTING_BIN, runner });
  assert.deepEqual(calls[0], ["domains", "list", "--repo", "E:/x/Pagus", "--json"]);
}

// ── 空 → autoDraft で draft → 再 list ───────────────────────────────────────
{
  const empty = JSON.stringify({ dir: "d", domains: [] });
  const { runner, calls } = makeRunner([empty, RESPONSE]);
  const domains = await fetchAnatomiaDomains({ project: "p" }, { binPath: EXISTING_BIN, runner, autoDraft: true });
  assert.equal(domains.length, 2, "draft 後の再 list で取得");
  assert.equal(calls[0][1], "list");
  assert.equal(calls[1][1], "draft");
  assert.ok(calls[1].includes("--no-llm"), "auto draft は既定で LLM を使わない");
  assert.equal(calls[2][1], "list");
}

// ── 空 + autoDraft=false → 空のまま (draft 走らない) ────────────────────────
{
  const empty = JSON.stringify({ dir: "d", domains: [] });
  const { runner, calls } = makeRunner([empty]);
  const domains = await fetchAnatomiaDomains({ project: "p" }, { binPath: EXISTING_BIN, runner, autoDraft: false });
  assert.equal(domains.length, 0);
  assert.equal(calls.length, 1, "list のみ・draft なし");
}

// ── exit!=0 → throw ─────────────────────────────────────────────────────────
{
  const runner: AnatomiaCliRunner = { async run() { return { stdout: "", exitCode: 2 }; } };
  await assert.rejects(
    fetchAnatomiaDomains({ project: "p" }, { binPath: EXISTING_BIN, runner }),
    /list 失敗 \(exit 2\)/
  );
}

// ── JSON 不正 → throw ───────────────────────────────────────────────────────
{
  const runner: AnatomiaCliRunner = { async run() { return { stdout: "not json", exitCode: 0 }; } };
  await assert.rejects(
    fetchAnatomiaDomains({ project: "p" }, { binPath: EXISTING_BIN, runner }),
    /JSON ではありません|domains 配列がありません/
  );
}

// ── bin 不在 → throw (fail-fast) ────────────────────────────────────────────
{
  const runner: AnatomiaCliRunner = { async run() { return { stdout: RESPONSE, exitCode: 0 }; } };
  await assert.rejects(
    fetchAnatomiaDomains({ project: "p" }, { binPath: "Z:/no/such/anatomia.mjs", runner }),
    /CLI が見つかりません/
  );
}

console.log("anatomia-client tests: passed");
