// investigate.ts (loadThemeMechanics / investigateTheme の件数) の Node 実装の入出力を記録する。
// 実行: npx tsx native/tests/golden/w1-E-investigate.gen.ts → native/tests/fixtures/flow/w1e_investigate.json
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { investigateTheme, loadThemeMechanics } from "../../../src/flow/investigate.js";

const files: Record<string, string> = {
  "death-stranding.md": ["---", "id: death-stranding", 'title: "DEATH STRANDING"', "genre: action", "mechanics:", "  - name: 積荷のバランス管理", "    description: 総重量と積み付けがサムの歩行安定に影響する", "    intended_affect: 重荷感", "---"].join("\n"),
  "journey.md": ["---", "id: journey", 'title: "Journey (風ノ旅ビト)"', "mechanics:", "  - name: スカーフと飛行", "    description: スカーフが浮遊の燃料になる", "    intended_affect: 高揚", "    intended_aspects: [fun, graphics, 3]", "    intended_emotions: [joy]", "    source: crawl", "  - name: ''", "  - description: 名前なし", "  - name: 協力", "    source: weird", "    intended_valence: positive", "---", "本文"].join("\n"),
  "monster-strike.md": ["---", "id: monster-strike", "title: モンスターストライク", "genre: パズル・RPG", "mechanics:", ...Array.from({ length: 12 }, (_, i) => `  - name: メカ${i}\n    description: 説明${i}`), "---"].join("\n"),
  "no-mech.md": "---\ntitle: 飛行機\n---\n",
  "plain.md": "メカニクスなし",
  "x.sentiment.md": "---\nmechanics:\n  - name: 飛行\n---\n",
  "journey.sentiment.json": JSON.stringify([1, 2, 3]),
  "monster-strike.sentiment.json": JSON.stringify({ comments: ["a", "b"] }),
  "broken.sentiment.json": "{",
  "notes.txt": "mechanics",
};
const dir = fs.mkdtempSync(path.join(os.tmpdir(), "w1e-inv-"));
for (const [name, content] of Object.entries(files)) fs.writeFileSync(path.join(dir, name), content);
const themes = ["Journey の飛行", "ローグライトの報酬設計", "飛行", "モンスターストライク", "monster", "death_stranding", "  ", "風ノ旅ビト", "(積荷)", "a", "パズル"];
const cases = [];
for (const theme of themes) {
  const r = await investigateTheme({ theme, tags: [], gamesDir: dir });
  cases.push({ theme, mechanics: loadThemeMechanics(dir, theme), sentimentCount: r.sentimentCount });
}
fs.rmSync(dir, { recursive: true, force: true });
const out = path.join(path.dirname(new URL(import.meta.url).pathname), "..", "fixtures", "flow", "w1e_investigate.json");
fs.writeFileSync(out, JSON.stringify({ files, cases }, null, 2) + "\n");
console.log("wrote w1e_investigate.json");
