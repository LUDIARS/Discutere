// Affect 語彙リゾルバ (Phase 0, 依存ゼロ)。
// 観測された感情語を統制語彙 (data/affects/vocabulary.json) の key に解決する。
// 既知 → key を返す。未知 → 「計算せず」単語登録質問を data/affects/pending-questions.json に積む。
//   (ベクトル化に必要な意味は後段 scripts/investigate-affects.mjs が LLM に調査させる。)
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const VOCAB = path.resolve(HERE, "../../../data/affects/vocabulary.json");
const QUEUE = path.resolve(HERE, "../../../data/affects/pending-questions.json");

const norm = (s) => (s || "").trim().toLowerCase();
const hash = (s) => { let h = 0; for (const c of s) h = (h * 31 + c.charCodeAt(0)) >>> 0; return h.toString(36); };
// ASCII slug を作る。日本語等で空になる場合はハッシュで一意化 (衝突回避)。
const slugId = (s) => { const b = norm(s).replace(/[^a-z0-9]+/g, "-").replace(/^-|-$/g, "").slice(0, 32); return "q_" + (b || hash(norm(s))); };

function loadVocab() {
  const v = JSON.parse(fs.readFileSync(VOCAB, "utf8").replace(/^﻿/, ""));
  const byKey = new Map(); const byLabel = new Map();
  for (const a of v.affects) { byKey.set(norm(a.key), a.key); if (a.label_ja) byLabel.set(norm(a.label_ja), a.key); }
  return { byKey, byLabel };
}
function loadQueue() {
  if (!fs.existsSync(QUEUE)) return { _note: "未知語の単語登録質問キュー。investigate-affects.mjs が LLM 調査して vocabulary.json に provisional 追加する。", questions: [] };
  return JSON.parse(fs.readFileSync(QUEUE, "utf8").replace(/^﻿/, ""));
}
const saveQueue = (q) => fs.writeFileSync(QUEUE, JSON.stringify(q, null, 2));

// term: 観測された感情語 (key/label_ja/任意の語)。context: 出所メモ (任意)。
// 戻り値: { status:"known", key } | { status:"queued", id, term }
export function resolveAffect(term, context = null) {
  const { byKey, byLabel } = loadVocab();
  const n = norm(term);
  const key = byKey.get(n) || byLabel.get(n);
  if (key) return { status: "known", key };

  // 未知語: 計算せずキューへ (重複は context を追記)
  const q = loadQueue();
  const id = slugId(term);
  let item = q.questions.find((x) => x.id === id);
  if (!item) {
    item = { id, term: String(term).trim(), status: "queued", contexts: [], proposed: null };
    q.questions.push(item);
  }
  if (context && !item.contexts.includes(context)) item.contexts.push(context);
  saveQueue(q);
  return { status: "queued", id, term: item.term };
}

// CLI: node affect-resolver.mjs <term> [context]  (動作確認用)
if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const [, , term, ctx] = process.argv;
  if (!term) { console.error("usage: node affect-resolver.mjs <term> [context]"); process.exit(1); }
  console.log(JSON.stringify(resolveAffect(term, ctx || null)));
}
