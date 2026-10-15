/**
 * scripts/link-file-deps.mjs の計画部 (planFileDependencyLinks) の単体テスト。
 * 依存表へ後から足した file: パッケージ (@ludiars/one-shot) が setup:submodules だけで
 * リンクされず ERR_MODULE_NOT_FOUND になった不具合の回帰。
 */

import assert from "node:assert/strict";
import path from "node:path";
import { test } from "node:test";

import { planFileDependencyLinks } from "../../scripts/link-file-deps.mjs";

const root = path.resolve("/repo");
const manifest = {
  dependencies: {
    "@ludiars/one-shot": "file:lib/lapilli/packages/one-shot",
    "@ludiars/llm-gateway": "file:lib/lapilli/packages/llm-gateway",
    hono: "^4.0.0",
  },
  devDependencies: { "@ludiars/vestigium": "file:lib/vestigium" },
};

function existsFrom(paths: string[]): (p: string) => boolean {
  const set = new Set(paths.map((p) => path.resolve(root, p)));
  return (p) => set.has(path.resolve(p));
}

test("plans a link for a file: dependency whose link is missing", () => {
  const exists = existsFrom([
    "lib/lapilli/packages/one-shot/package.json",
    "lib/lapilli/packages/llm-gateway/package.json",
    "node_modules/@ludiars/llm-gateway",
    "lib/vestigium/package.json",
    "node_modules/@ludiars/vestigium",
  ]);
  const plans = planFileDependencyLinks(manifest, root, exists);
  assert.deepEqual(plans, [
    {
      name: "@ludiars/one-shot",
      target: path.resolve(root, "lib/lapilli/packages/one-shot"),
      link: path.join(root, "node_modules", "@ludiars", "one-shot"),
    },
  ]);
});

test("skips registry dependencies, existing links and missing targets", () => {
  const exists = existsFrom(["node_modules/@ludiars/one-shot"]);
  assert.deepEqual(planFileDependencyLinks(manifest, root, exists), []);
});
