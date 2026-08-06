// レビュー/コメント収集物 → 複合固定ベクトル(再定義版) + 感情/議論データへ変換 (Phase 0, 依存ゼロ)。
//
// 「用意されたベクトル」= 複合固定 20 次元 (全ゲーム共通・各次元 0..1)。各収集集合をこの空間へ正規化する。
//   emo.valence, emo.arousal,
//   emo.{joy,trust,fear,surprise,sadness,disgust,anger,anticipation}   (Plutchik 8)
//   asp.{fun,difficulty,content,price_value,performance,story,graphics,replayability}
//   meta.positive_ratio, meta.volume_log
// ゲーム全体・議論クラスタ・月次バケットを全て同一 20 次元で表現 → cosine 比較・時系列(感情曲線)可能。
// number[] なので Discatier embeddings(vector_json) に格納可、emo ブロックは affects(mood/valence/score) に対応。
//
// 仕様準拠(spec/feature/crawler/DESIGN.md): summary-only(原文非転載) / author 非保管 / sources[].fetched_at。
// ベクトルは外部 embedding API を使わず本コード内(Claude Code)で算出。
//
// 使い方:
//   node src/crawler/sentiment/analyze.mjs --in <collected.json> --slug <slug> --title "<Title>" [--genre <g>] [--out data/games]
import fs from "fs";
import path from "path";
import { assertCrawlerVectorSpec } from "./vector-spec.mjs";

const lex = JSON.parse(fs.readFileSync(new URL("./lexicon.json", import.meta.url), "utf8"));
const EMO = Object.keys(lex.emotions); // 8
const ASP = Object.keys(lex.aspects);  // 8
export const VECTOR_SPEC = [
  "emo.valence", "emo.arousal",
  ...EMO.map(e => "emo." + e),
  ...ASP.map(a => "asp." + a),
  "meta.positive_ratio", "meta.volume_log",
]; // 20 dims, 0..1
export const VECTOR_SPEC_VERSION = 1;
assertCrawlerVectorSpec(VECTOR_SPEC, VECTOR_SPEC_VERSION);

const arg = (n, d = null) => { const i = process.argv.indexOf("--" + n); return i >= 0 ? process.argv[i + 1] : d; };
const n01 = v => Math.max(0, Math.min(1, v));
const r4 = v => +v.toFixed(4);

// 1 レコードの素性
function features(text, meta = {}) {
  const t = (text || "").toLowerCase();
  const hit = ws => ws.reduce((s, w) => s + (t.includes(w.toLowerCase()) ? 1 : 0), 0);
  let pol = 0, polHits = 0;
  for (const [w, sc] of Object.entries(lex.polarity)) if (t.includes(w.toLowerCase())) { pol += sc; polHits++; }
  if (meta.voted_up === true) { pol += 1.5; polHits++; } else if (meta.voted_up === false) { pol -= 1.5; polHits++; }
  const valence = polHits ? Math.max(-1, Math.min(1, pol / Math.max(2, polHits))) : 0;
  const emo = {}; for (const e of EMO) emo[e] = hit(lex.emotions[e]) > 0 ? 1 : 0;
  const aspMent = {}; for (const a of ASP) aspMent[a] = hit(lex.aspects[a]);
  const arousal = n01((hit(lex.arousal) + ((text || "").split("!").length - 1)) / 4);
  return { valence, emo, aspMent, arousal };
}

// 素性集合 → 複合 20 次元ベクトル(0..1)
function buildVector(fl) {
  const n = fl.length || 1;
  const meanVal = fl.reduce((s, f) => s + f.valence, 0) / n;
  const arousal = fl.reduce((s, f) => s + f.arousal, 0) / n;
  const emo = EMO.map(e => fl.reduce((s, f) => s + f.emo[e], 0) / n);
  const asp = ASP.map(a => {
    let pos = 0, neg = 0, m = 0;
    for (const f of fl) if (f.aspMent[a]) { m++; if (f.valence > 0.05) pos++; else if (f.valence < -0.05) neg++; }
    return m ? n01(0.5 + 0.5 * (pos - neg) / m) : 0.5;
  });
  const positive_ratio = fl.filter(f => f.valence > 0.05).length / n;
  const volume_log = n01(Math.log10(fl.length + 1) / 4);
  return [n01((meanVal + 1) / 2), arousal, ...emo, ...asp, positive_ratio, volume_log].map(r4);
}
const moodOf = fl => { const c = {}; for (const e of EMO) c[e] = fl.reduce((s, f) => s + f.emo[e], 0); let b = "neutral", mx = 0; for (const [e, v] of Object.entries(c)) if (v > mx) { mx = v; b = e; } return mx ? b : "neutral"; };
const valLabel = v => (v > 0.55 ? "positive" : v < 0.45 ? "negative" : "neutral"); // v は 0..1 (emo.valence)

function analyze(collected) {
  const fl = (collected.records || []).map(r => ({ posted_at: r.posted_at, ...features(r.text, r.meta || {}) }));
  const gameVec = buildVector(fl);
  const obj = vec => Object.fromEntries(VECTOR_SPEC.map((k, i) => [k, vec[i]]));
  const gv = obj(gameVec);

  // aspects (mentions 付き)
  const aspects = {};
  ASP.forEach((a, i) => {
    const m = fl.filter(f => f.aspMent[a]).length;
    aspects[a] = { mentions: m, score: gameVec[2 + EMO.length + i] };
  });

  // 感情曲線 (月次、各バケットも同一 20 次元)
  const buckets = {};
  for (const f of fl) (buckets[(f.posted_at || "").slice(0, 7) || "unknown"] ||= []).push(f);
  const sentiment_curve = Object.keys(buckets).sort().map(period => {
    const v = buildVector(buckets[period]);
    return { period, count: buckets[period].length, valence: v[0], arousal: v[1], mood: moodOf(buckets[period]), sentiment: valLabel(v[0]), vector: v };
  });

  // affects (Discatier affect 対応)
  const affects = [
    { subject: "overall", mood: moodOf(fl), valence: valLabel(gameVec[0]), score: gameVec[0] },
    ...sentiment_curve.map(b => ({ subject: `period:${b.period}`, mood: b.mood, valence: b.sentiment, score: b.valence })),
  ];

  // discussion clusters (dominant aspect 別、同一 20 次元ベクトル)
  const byAspect = {};
  for (const f of fl) {
    let dom = null, mx = 0;
    for (const a of ASP) if (f.aspMent[a] > mx) { mx = f.aspMent[a]; dom = a; }
    if (dom) (byAspect[dom] ||= []).push(f);
  }
  const clusters = Object.entries(byAspect).map(([a, sub]) => {
    const v = buildVector(sub);
    return { id: `cluster:${a}`, topic_aspect: a, size: sub.length, sentiment: valLabel(v[0]), score: v[0], vector: v };
  }).sort((x, y) => y.size - x.size);

  return {
    overall: { valence: valLabel(gameVec[0]), score: gameVec[0], positive_ratio: gv["meta.positive_ratio"], volume: fl.length, volume_log: gv["meta.volume_log"] },
    game_vector: gameVec, aspects, affects, sentiment_curve, clusters,
  };
}

const SOURCE_META = {
  steam: { title: "Steam User Reviews", attribution: "Valve / Steam users", url: "https://store.steampowered.com/" },
  youtube: { title: "YouTube Comments", attribution: "YouTube users", url: "https://www.youtube.com/" },
  google: { title: "Google Search (Custom Search API)", attribution: "various", url: "https://www.google.com/" },
  gamewith: { title: "GameWith Comments", attribution: "GameWith users", url: "https://gamewith.jp/" },
};
const sourcesYaml = c => (c.by_source || []).filter(s => s.status === "ok").map(s => {
  const m = SOURCE_META[s.source] || { title: s.source, attribution: s.source, url: "" };
  const d = c.fetched_date || new Date().toISOString().slice(0, 10);
  return `  - url: "${m.url}"\n    title: "${m.title}"\n    fetched_at: "${d}"\n    attribution: "${m.attribution}"\n    excerpt_policy: "summary-only"`;
}).join("\n");

function writeMd(outDir, slug, title, genre, c, res) {
  const asp = Object.entries(res.aspects).filter(([, a]) => a.mentions).sort((a, b) => b[1].mentions - a[1].mentions)
    .map(([a, v]) => `- ${a}: score ${v.score} (言及 ${v.mentions})`).join("\n");
  const body = `# 概要\n\n${title} のユーザー反応サマリ (収集 ${res.overall.volume} 件・${c.fetched_date})。\n総合感情: **${res.overall.valence}** (score ${res.overall.score} / positive率 ${(res.overall.positive_ratio * 100).toFixed(0)}%)。\n\n# アスペクト評価 (要約)\n\n${asp}\n\n※ ソース本文は転載せず数値・ラベル・ベクトルのみ (summary-only)。複合ベクトル/感情曲線/議論クラスタは ${slug}.sentiment.json 参照。\n`;
  const md = `---\nid: ${slug}\ntitle: "${title}"\ngenre: "${genre || ""}"\nworkspace_id: "knowledge"\nsources:\n${sourcesYaml(c)}\nmechanics: []\naesthetics: []\n---\n\n${body}`;
  fs.mkdirSync(outDir, { recursive: true });
  fs.writeFileSync(path.join(outDir, `${slug}.md`), md);
}

const inPath = arg("in"), slug = arg("slug"), title = arg("title"), genre = arg("genre", ""), outDir = arg("out", "data/games");
if (!inPath || !slug || !title) { console.error('usage: node analyze.mjs --in <collected.json> --slug <slug> --title "<Title>" [--genre <g>] [--out data/games]'); process.exit(1); }
const collected = JSON.parse(fs.readFileSync(inPath, "utf8"));
const res = analyze(collected);
writeMd(outDir, slug, title, genre, collected, res);
const side = { game: title, slug, fetched_date: collected.fetched_date,
  method: "hybrid (lexicon first-pass; composite fixed vector computed in-code / Claude Code, no external embedding API)",
  vector_spec: VECTOR_SPEC, vector_spec_version: VECTOR_SPEC_VERSION, vector_range: "0..1",
  sources: (collected.by_source || []).filter(s => s.status === "ok").map(s => ({ source: s.source, n: s.n })), ...res };
fs.writeFileSync(path.join(outDir, `${slug}.sentiment.json`), JSON.stringify(side, null, 2));
console.log(`wrote ${outDir}/${slug}.md + ${slug}.sentiment.json  (vector dims=${VECTOR_SPEC.length})`);
console.log(`overall: ${res.overall.valence} (${res.overall.score}) positive=${(res.overall.positive_ratio * 100).toFixed(0)}% vol=${res.overall.volume}`);
console.log(`clusters: ${res.clusters.map(c => `${c.topic_aspect}(${c.size},${c.sentiment})`).join(", ")}`);
