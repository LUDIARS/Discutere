// 未知 Affect 語の LLM 調査ワーカー (依存ゼロ / Claude Code 方針: LLM に意味を調査させる)。
//   data/affects/pending-questions.json の queued を LLM に投げ、
//   {key,label_ja,valence,description} を得て data/affects/vocabulary.json に
//   status="provisional", provenance="llm" で追加。質問は status="answered" に。
//   provisional 語は後で人手修正可能 (vocabulary.json を編集 → npm run seed:affects)。
//
// 実行: ANTHROPIC_API_KEY=... node scripts/investigate-affects.mjs [--limit N]
//   キー未設定なら queued 件数を表示して no-op (安全 default)。
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const VOCAB = path.resolve(HERE, "../data/affects/vocabulary.json");
const QUEUE = path.resolve(HERE, "../data/affects/pending-questions.json");
const MODEL = process.env.ANTHROPIC_MODEL || "claude-haiku-4-5-20251001";
const arg = (n, d) => { const i = process.argv.indexOf("--" + n); return i >= 0 ? process.argv[i + 1] : d; };

const readJson = (p) => JSON.parse(fs.readFileSync(p, "utf8").replace(/^﻿/, ""));
const writeJson = (p, o) => fs.writeFileSync(p, JSON.stringify(o, null, 2));
const slug = (s) => s.toLowerCase().normalize("NFKD").replace(/[^a-z0-9]+/g, "_").replace(/^_|_$/g, "").slice(0, 40);

async function ask(term, contexts, knownKeys) {
  const prompt = `あなたはゲーム体験の感情語彙を整備するアシスタントです。\n` +
    `次の未知の感情語を、統制語彙に登録するための情報に変換してください。\n` +
    `語: "${term}"\n文脈: ${contexts.length ? contexts.join(" / ") : "(なし)"}\n` +
    `既存 key (重複回避用・近ければ既存を再利用可): ${knownKeys.join(", ")}\n\n` +
    `JSON のみで出力: {"key":"英snake","label_ja":"日本語名","valence":"positive|negative|ambivalent","description":"1文の定義","alias_of":"既存keyに統合できるなら其のkey/無ければnull"}`;
  const res = await fetch("https://api.anthropic.com/v1/messages", {
    method: "POST",
    headers: { "content-type": "application/json", "x-api-key": process.env.ANTHROPIC_API_KEY, "anthropic-version": "2023-06-01" },
    body: JSON.stringify({ model: MODEL, max_tokens: 300, messages: [{ role: "user", content: prompt }] }),
  });
  const j = await res.json();
  if (j.error) throw new Error(j.error.message);
  const text = (j.content || []).map((c) => c.text || "").join("");
  const m = text.match(/\{[\s\S]*\}/);
  if (!m) throw new Error("LLM 応答に JSON なし: " + text.slice(0, 120));
  return JSON.parse(m[0]);
}

(async () => {
  const queue = readJson(QUEUE);
  const queued = queue.questions.filter((q) => q.status === "queued");
  if (!process.env.ANTHROPIC_API_KEY) {
    console.log(`queued=${queued.length} (ANTHROPIC_API_KEY 未設定のため調査スキップ)。設定して再実行で provisional 登録。`);
    return;
  }
  const vocab = readJson(VOCAB);
  const knownKeys = vocab.affects.map((a) => a.key);
  const limit = Number(arg("limit", queued.length));
  let added = 0;
  for (const q of queued.slice(0, limit)) {
    try {
      const r = await ask(q.term, q.contexts || [], knownKeys);
      if (r.alias_of && knownKeys.includes(r.alias_of)) {
        q.status = "answered"; q.proposed = { alias_of: r.alias_of };
      } else {
        const key = knownKeys.includes(r.key) ? r.key : (r.key || slug(q.term));
        if (!vocab.affects.some((a) => a.key === key)) {
          vocab.affects.push({ key, label_ja: r.label_ja || q.term, valence: r.valence || "ambivalent",
            description: r.description || "", status: "provisional", provenance: "llm" });
          knownKeys.push(key); added += 1;
        }
        q.status = "answered"; q.proposed = { key, valence: r.valence };
      }
    } catch (e) { q.status = "error"; q.error = e.message; }
    await new Promise((res) => setTimeout(res, 300));
  }
  writeJson(VOCAB, vocab);
  writeJson(QUEUE, queue);
  console.log(`investigated ${Math.min(limit, queued.length)} / added provisional ${added}。vocabulary.json を確認・修正後 npm run seed:affects。`);
})().catch((e) => { console.error("ERR", e.message); process.exit(1); });
