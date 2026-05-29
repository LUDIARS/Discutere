// game-knowledge-graph の Title KG (graph/titles/*.json) を Discatier GameKG md に変換する。
//   systems → mechanics (intended_affect は Affect 語彙 key に発見的マッピング)。
//   出力: data/games/<slug>.md (importer 互換 frontmatter)。
// 実行: node scripts/import-kg-titles.mjs [--src ../game-knowledge-graph/graph/titles] [--out data/games]
import fs from "node:fs";
import path from "node:path";

const arg = (n, d) => { const i = process.argv.indexOf("--" + n); return i >= 0 ? process.argv[i + 1] : d; };
const SRC = path.resolve(arg("src", "../game-knowledge-graph/graph/titles"));
const OUT = path.resolve(arg("out", "data/games"));

// system 名/目的のキーワード → Affect 語彙 key (data/affects/vocabulary.json)
const AFFECT_RULES = [
  [/自動攻撃|weapon|武器|攻撃/i, "game_feel"],
  [/レベルアップ|level|天賦|突破|育成|熟練|スキル/i, "mastery"],
  [/進化|evolution|アンロック|解放/i, "accomplishment"],
  [/パッシブ|アルカナ|arcana|戦略|戦術|パーティ|切替|デッキ|編成/i, "strategic"],
  [/生存|タイマー|ステージ|難易度|回避|無敵|スタミナ/i, "tension_release"],
  [/収集|ピック|ゴールド|コイン|聖遺物|ガチャ|祈願|宝/i, "collection"],
  [/探索|オープンワールド|発見|地域|マップ/i, "discovery"],
  [/キャラ|character|自己表現|ビルド|装飾/i, "expression"],
  [/ストーリー|物語|シナリオ|世界観|narrative/i, "narrative"],
];
const affectFor = (sys) => {
  const hay = `${sys.name} ${sys.purpose || ""}`;
  for (const [re, key] of AFFECT_RULES) if (re.test(hay)) return key;
  return "immersion";
};

const FANDOM = {
  "src_0002": { url: "https://vampiresurvivors.fandom.com/wiki/Vampire_Survivors", title: "Vampire Survivors Wiki" },
  "src_0003": { url: "https://genshin-impact.fandom.com/wiki/Genshin_Impact", title: "Genshin Impact Wiki" },
};

function toMd(t) {
  const g = t.title;
  const slug = g.id.replace(/^title:/, "");
  const genre = (g.genres && g.genres[0]) || "";
  const src = FANDOM[t.import_id] || { url: "", title: t.import_id || "source" };
  const mechanics = t.systems.map((s) => {
    const desc = (s.purpose || "").replace(/"/g, "'") + (s.note ? ` (${s.note.replace(/"/g, "'")})` : "");
    return `  - name: "${s.name.replace(/"/g, "'")}"\n    description: "${desc}"\n    intends: "${(s.purpose || "").replace(/"/g, "'").slice(0, 60)}"\n    intended_affect: "${affectFor(s)}"`;
  }).join("\n");
  const body = `# 概要\n\n${(g.name)} のメカニクス KG (game-knowledge-graph ${t.import_id} 由来)。\nユーザー反応 (感情曲線/議論クラスタ) は ${slug}.sentiment.json 参照。\n`;
  const md = `---\nid: ${slug}\ntitle: "${g.name.replace(/"/g, "'")}"\ngenre: "${genre}"\nworkspace_id: "knowledge"\nsources:\n  - url: "${src.url}"\n    title: "${src.title}"\n    fetched_at: "2026-05-29"\n    attribution: "CC BY-SA (Fandom)"\n    excerpt_policy: "summary-only"\nmechanics:\n${mechanics}\naesthetics: []\n---\n\n${body}`;
  return { slug, md, count: t.systems.length };
}

fs.mkdirSync(OUT, { recursive: true });
const files = fs.readdirSync(SRC).filter((f) => f.endsWith(".json"));
for (const f of files) {
  const t = JSON.parse(fs.readFileSync(path.join(SRC, f), "utf8").replace(/^﻿/, ""));
  if (!t.title || !t.systems) { console.log("skip (not a title KG):", f); continue; }
  const { slug, md, count } = toMd(t);
  fs.writeFileSync(path.join(OUT, `${slug}.md`), md);
  console.log(`wrote ${OUT}/${slug}.md  (mechanics=${count})`);
}
