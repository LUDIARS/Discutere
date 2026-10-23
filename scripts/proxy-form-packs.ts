/**
 * 公開レビュー (steam-persona 等が出す ExternalUtterance JSONL) から人物パックを作る
 * (spec/feature/flow/proxy-debate.md §4。Histrio の形成層ができるまでの仮置き = 統合レビュー U1)。
 *
 *   npm run proxy:form -- --input <file.jsonl|dir> --genres <genres.json> --out <packs.jsonl> [--heldout-out <heldout.jsonl>]
 *   npm run proxy:form -- --input <...> --genres <...> --dry     (パック数・材料の充足度だけ表示)
 *
 * 仮名の鍵は env DISCUTERE_PROXY_SPEAKER_HMAC_KEY (Ex Vault)。無ければ止まる。
 * LLM は使わない。出力は proxy:import にそのまま渡せる。
 */

import { readdirSync, readFileSync, statSync, writeFileSync } from "node:fs";
import path from "node:path";

import type { ExternalUtterance } from "../src/crawler/sources/types.js";
import { formPersonaPacks } from "../src/flow/proxy/form/group.js";
import { parseGenreDictionary } from "../src/flow/proxy/form/genres.js";

function valueOf(flag: string): string | undefined {
  const index = process.argv.indexOf(flag);
  const value = index >= 0 ? process.argv[index + 1] : undefined;
  return value && !value.startsWith("--") ? value : undefined;
}

function readJsonl(file: string): ExternalUtterance[] {
  return readFileSync(file, "utf8")
    .split(/\r?\n/)
    .filter((l) => l.trim())
    .flatMap((l) => {
      try {
        return [JSON.parse(l) as ExternalUtterance];
      } catch {
        return [];
      }
    });
}

function readInput(p: string): ExternalUtterance[] {
  const abs = path.resolve(p);
  if (statSync(abs).isDirectory()) {
    return readdirSync(abs)
      .filter((f) => f.endsWith(".jsonl"))
      .sort()
      .flatMap((f) => readJsonl(path.join(abs, f)));
  }
  return readJsonl(abs);
}

const input = valueOf("--input");
const genres = valueOf("--genres");
const out = valueOf("--out");
const heldOutOut = valueOf("--heldout-out");
const dry = process.argv.includes("--dry");
if (!input || !genres || (!out && !dry)) {
  console.error("usage: npm run proxy:form -- --input <file.jsonl|dir> --genres <genres.json> (--out <packs.jsonl> | --dry)");
  process.exit(2);
}
const hmacKey = process.env.DISCUTERE_PROXY_SPEAKER_HMAC_KEY ?? "";
if (!hmacKey) {
  console.error("DISCUTERE_PROXY_SPEAKER_HMAC_KEY が未設定です (Ex Vault で設定してください)");
  process.exit(2);
}

const { packs, heldOutTexts, report } = formPersonaPacks({
  utterances: readInput(input),
  dictionary: parseGenreDictionary(JSON.parse(readFileSync(path.resolve(genres), "utf8"))),
  hmacKey,
  now: Date.now(),
});
console.log(
  `proxy:form 話者 ${report.speakers} 人 / パック ${report.packs} 件 ` +
    `(high ${report.byConfidence.high} / medium ${report.byConfidence.medium} / low ${report.byConfidence.low})`
);
console.log(
  `  使わなかったレビュー: 推薦可否なし ${report.skipped.noSignal} / 短すぎる ${report.skipped.tooShort} / ` +
    `辞書に無いゲーム ${report.skipped.unclassified} / 書き手不明 ${report.skipped.noAuthor}`
);
if (!dry && out) {
  writeFileSync(path.resolve(out), packs.map((p) => JSON.stringify(p)).join("\n") + (packs.length ? "\n" : ""), "utf8");
  console.log(`  → ${out}`);
  if (heldOutOut) {
    // 評価専用 (eval:proxy の判別テスト)。議論や取込には渡さない。
    writeFileSync(path.resolve(heldOutOut), heldOutTexts.map((h) => JSON.stringify(h)).join("\n") + (heldOutTexts.length ? "\n" : ""), "utf8");
    console.log(`  → ${heldOutOut} (評価専用の伏せた発言 ${heldOutTexts.length} 件)`);
  }
}
