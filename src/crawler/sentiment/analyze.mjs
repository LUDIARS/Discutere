// レビュー/コメント収集物 → Discatier 互換の感情/議論データへ変換 (Phase 0, 依存ゼロ)。
//
// 入力: collectors (game-knowledge-graph) の collected.json
//        { game, fetched_date, by_source[], records[]{source,text,lang,posted_at,meta{voted_up,...}} }
// 出力:
//   data/games/<slug>.md            ... importer 互換 frontmatter (Game/genre/sources) + 人間用サマリ
//   data/games/<slug>.sentiment.json ... affects(感情曲線) / aspects / discussion clusters(L2正規化ベクトル)
//
// 仕様準拠 (spec/crawler/DESIGN.md):
//   - sources[].excerpt_policy = "summary-only"。レビュー原文は md/JSON に転載しない (数値・ラベルのみ)。
//   - 個人データ (author) は出力しない。
//   - 議論はベクトルデータ: クラスタを 16 次元 (emotion8 + aspect8) の特徴ベクトルにして L2 正規化
//     → Discatier embeddings(node_type="discussion_cluster", vector_json:number[]) として後段 import 可能。
//     ベクトル生成は外部 embedding API を使わず本コード (Claude Code) 内で算出する。
//
// 使い方:
//   node src/crawler/sentiment/analyze.mjs --in <collected.json> --slug <slug> --title "<Title>" --genre "<genre>" [--out data/games]
import fs from "fs";
import path from "path";

const lex = JSON.parse(fs.readFileSync(new URL("./lexicon.json", import.meta.url), "utf8"));
const EMO = Object.keys(lex.emotions);                 // 8 emotions (Plutchik)
const ASP = Object.keys(lex.aspects);                  // 8 game-review aspects
export const EMBED_SPACE = [...EMO.map(e => "emo." + e), ...ASP.map(a => "asp." + a)]; // 16 dims

const arg = (n, d = null) => { const i = process.argv.indexOf("--" + n); return i >= 0 ? process.argv[i + 1] : d; };
const norm01 = v => Math.max(0, Math.min(1, v));
const l2 = vec => { const n = Math.sqrt(vec.reduce((s, x) => s + x * x, 0)) || 1; return vec.map(x => +(x / n).toFixed(6)); };

// 1 レコードの素性を辞書で抽出
function features(text, meta = {}) {
  const t = (text || "").toLowerCase();
  const hit = words => words.reduce((s, w) => s + (t.includes(w.toLowerCase()) ? 1 : 0), 0);
  // 極性: 辞書スコア合計 + Steam voted_up の強シグナル
  let pol = 0, polHits = 0;
  for (const [w, sc] of Object.entries(lex.polarity)) if (t.includes(w.toLowerCase())) { pol += sc; polHits++; }
  if (meta.voted_up === true) { pol += 1.5; polHits++; }
  else if (meta.voted_up === false) { pol -= 1.5; polHits++; }
  const valence = polHits ? Math.max(-1, Math.min(1, pol / Math.max(2, polHits))) : 0;
  const emo = {}; for (const e of EMO) emo[e] = hit(lex.emotions[e]);
  const asp = {}; for (const a of ASP) asp[a] = { mentions: hit(lex.aspects[a]) };
  const arousal = norm01((hit(lex.arousal) + (text || "").split("!").length - 1) / 4);
  return { valence, emo, asp, arousal };
}

const moodOf = emoCounts => { let best = "neutral", n = 0; for (const [e, c] of Object.entries(emoCounts)) if (c > n) { n = c; best = e; } return n ? best : "neutral"; };
const valLabel = v => (v > 0.15 ? "positive" : v < -0.15 ? "negative" : "neutral");

function analyze(collected) {
  const recs = collected.records || [];
  const per = recs.map(r => ({ posted_at: r.posted_at, source: r.source, f: features(r.text, r.meta || {}) }));

  // ---- aspects 集約 (0..1, 0.5=中立) ----
  const aspects = {};
  for (const a of ASP) {
    let pos = 0, neg = 0, m = 0;
    for (const p of per) if (p.f.asp[a].mentions) { m++; if (p.f.valence > 0.05) pos++; else if (p.f.valence < -0.05) neg++; }
    aspects[a] = { mentions: m, score: m ? +norm01(0.5 + 0.5 * (pos - neg) / m).toFixed(4) : 0.5 };
  }

  // ---- 感情曲線 (月次バケット) ----
  const buckets = {};
  for (const p of per) {
    const period = (p.posted_at || "").slice(0, 7) || "unknown";
    (buckets[period] ||= []).push(p.f);
  }
  const sentiment_curve = Object.keys(buckets).sort().map(period => {
    const fs_ = buckets[period];
    const valence = fs_.reduce((s, f) => s + f.valence, 0) / fs_.length;
    const emoSum = {}; for (const e of EMO) emoSum[e] = fs_.reduce((s, f) => s + f.emo[e], 0);
    return { period, count: fs_.length, valence01: +norm01((valence + 1) / 2).toFixed(4),
      mood: moodOf(emoSum), sentiment: valLabel(valence) };
  });

  // ---- affects (Discatier affect: mood/score/valence) ----
  const overallVal = per.reduce((s, p) => s + p.f.valence, 0) / (per.length || 1);
  const emoTotals = {}; for (const e of EMO) emoTotals[e] = per.reduce((s, p) => s + (p.f.emo[e] ? 1 : 0), 0);
  const affects = [
    { subject: "overall", mood: moodOf(emoTotals), valence: valLabel(overallVal), score: +norm01((overallVal + 1) / 2).toFixed(4) },
    ...sentiment_curve.map(b => ({ subject: `period:${b.period}`, mood: b.mood, valence: b.sentiment, score: b.valence01 })),
  ];

  // ---- discussion clusters (dominant aspect 別) + 16次元 L2 正規化ベクトル ----
  const byAspect = {};
  for (const p of per) {
    let dom = null, mx = 0;
    for (const a of ASP) if (p.f.asp[a].mentions > mx) { mx = p.f.asp[a].mentions; dom = a; }
    if (!dom) continue; (byAspect[dom] ||= []).push(p.f);
  }
  const clusters = Object.entries(byAspect).map(([asp, fs_]) => {
    const vecRaw = [
      ...EMO.map(e => fs_.reduce((s, f) => s + (f.emo[e] ? 1 : 0), 0) / fs_.length),
      ...ASP.map(a => fs_.reduce((s, f) => s + (f.asp[a].mentions ? 1 : 0), 0) / fs_.length),
    ];
    const v = fs_.reduce((s, f) => s + f.valence, 0) / fs_.length;
    return { id: `cluster:${asp}`, topic_aspect: asp, size: fs_.length,
      sentiment: valLabel(v), score: +norm01((v + 1) / 2).toFixed(4),
      vector: l2(vecRaw) };  // node_type=discussion_cluster の embeddings 用
  }).sort((a, b) => b.size - a.size);

  const positive = per.filter(p => p.f.valence > 0.05).length;
  return {
    overall: { valence: valLabel(overallVal), score: +norm01((overallVal + 1) / 2).toFixed(4),
      positive_ratio: +(positive / (per.length || 1)).toFixed(4), volume: per.length,
      volume_log: +norm01(Math.log10(per.length + 1) / 4).toFixed(4) },
    aspects, affects, sentiment_curve, clusters,
  };
}

// ---- sources frontmatter (excerpt_policy: summary-only) ----
const SOURCE_META = {
  steam: { title: "Steam User Reviews", attribution: "Valve / Steam users", url: "https://store.steampowered.com/" },
  youtube: { title: "YouTube Comments", attribution: "YouTube users", url: "https://www.youtube.com/" },
  google: { title: "Google Search (Custom Search API)", attribution: "various", url: "https://www.google.com/" },
  gamewith: { title: "GameWith Comments", attribution: "GameWith users", url: "https://gamewith.jp/" },
};
function sourcesYaml(collected) {
  const date = collected.fetched_date || new Date().toISOString().slice(0, 10);
  return (collected.by_source || []).filter(s => s.status === "ok").map(s => {
    const m = SOURCE_META[s.source] || { title: s.source, attribution: s.source, url: "" };
    return `  - url: "${m.url}"\n    title: "${m.title}"\n    fetched_at: "${date}"\n    attribution: "${m.attribution}"\n    excerpt_policy: "summary-only"`;
  }).join("\n");
}

function writeMd(outDir, slug, title, genre, collected, result) {
  const body = `# 概要\n\n${title} のユーザー反応サマリ (収集 ${result.overall.volume} 件・${collected.fetched_date})。\n` +
    `総合感情: **${result.overall.valence}** (score ${result.overall.score} / positive率 ${(result.overall.positive_ratio * 100).toFixed(0)}%)。\n\n` +
    `# アスペクト評価 (要約)\n\n` +
    Object.entries(result.aspects).filter(([, a]) => a.mentions).sort((a, b) => b[1].mentions - a[1].mentions)
      .map(([a, v]) => `- ${a}: score ${v.score} (言及 ${v.mentions})`).join("\n") +
    `\n\n※ ソース本文は転載せず数値・ラベルのみ (summary-only)。感情曲線/議論クラスタベクトルは ${slug}.sentiment.json 参照。\n`;
  const md = `---\nid: ${slug}\ntitle: "${title}"\ngenre: "${genre || ""}"\nworkspace_id: "knowledge"\nsources:\n${sourcesYaml(collected)}\nmechanics: []\naesthetics: []\n---\n\n${body}`;
  fs.mkdirSync(outDir, { recursive: true });
  fs.writeFileSync(path.join(outDir, `${slug}.md`), md);
}

// ---- main ----
const inPath = arg("in"); const slug = arg("slug"); const title = arg("title"); const genre = arg("genre", "");
const outDir = arg("out", "data/games");
if (!inPath || !slug || !title) { console.error('usage: node analyze.mjs --in <collected.json> --slug <slug> --title "<Title>" [--genre <g>] [--out data/games]'); process.exit(1); }

const collected = JSON.parse(fs.readFileSync(inPath, "utf8"));
const result = analyze(collected);
writeMd(outDir, slug, title, genre, collected, result);
const side = { game: title, slug, fetched_date: collected.fetched_date, method: "hybrid (lexicon first-pass; vectors generated in-code / Claude Code, no external embedding API)",
  embedding_space: EMBED_SPACE, sources: (collected.by_source || []).filter(s => s.status === "ok").map(s => ({ source: s.source, n: s.n })), ...result };
fs.mkdirSync(outDir, { recursive: true });
fs.writeFileSync(path.join(outDir, `${slug}.sentiment.json`), JSON.stringify(side, null, 2));

console.log(`wrote ${outDir}/${slug}.md + ${slug}.sentiment.json`);
console.log(`overall: ${result.overall.valence} (${result.overall.score}) positive=${(result.overall.positive_ratio * 100).toFixed(0)}% vol=${result.overall.volume}`);
console.log(`clusters: ${result.clusters.map(c => `${c.topic_aspect}(${c.size},${c.sentiment})`).join(", ")}`);
console.log(`curve periods: ${result.sentiment_curve.length}`);
