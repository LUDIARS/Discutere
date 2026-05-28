import { randomUUID } from "node:crypto";
import { ludusLearningJobRepo, ludusMechanicRepo } from "../db/repository.js";

export interface LearnMechanicsInput {
  workspaceId: string;
  gameTitle: string;
  query: string;
  limit?: number;
}

interface SearchArticle {
  title: string;
  link: string;
  snippet: string;
}

const KNOWN_MECHANICS = [
  "deckbuilding",
  "roguelike",
  "combo",
  "resource management",
  "turn-based",
  "real-time",
  "cooldown",
  "skill tree",
  "crafting",
  "gacha",
  "loot",
  "build order",
  "positioning",
  "status effect",
  "damage over time",
  "crowd control",
  "parry",
  "hitbox",
  "dodge",
  "metaprogression",
  "synergy",
  "area control",
  "economy",
  "fog of war",
  "quest routing",
];

export async function startMechanicsLearning(input: LearnMechanicsInput): Promise<{ jobId: string }> {
  const jobId = randomUUID();
  await ludusLearningJobRepo.create({
    id: jobId,
    workspaceId: input.workspaceId,
    gameTitle: input.gameTitle,
    query: input.query,
    status: "queued",
    fetchedCount: 0,
    learnedCount: 0,
    createdAt: new Date(),
    updatedAt: new Date(),
  });

  void runLearningJob(jobId, input).catch(async (err) => {
    await ludusLearningJobRepo.update(jobId, {
      status: "failed",
      error: (err as Error).message,
    });
  });

  return { jobId };
}

async function runLearningJob(jobId: string, input: LearnMechanicsInput): Promise<void> {
  await ludusLearningJobRepo.update(jobId, { status: "running", error: null });

  const limit = Math.max(1, Math.min(50, input.limit ?? 20));
  const articles = await searchGuideArticles(input.query, limit);
  let learned = 0;

  for (const article of articles) {
    const candidates = extractMechanics(article);
    for (const c of candidates) {
      await ludusMechanicRepo.create({
        id: randomUUID(),
        workspaceId: input.workspaceId,
        gameTitle: input.gameTitle,
        mechanic: c.mechanic,
        confidence: c.confidence,
        sourceUrl: article.link,
        sourceTitle: article.title,
        sourceSnippet: article.snippet,
        tagsJson: JSON.stringify(c.tags),
        createdAt: new Date(),
      });
      learned += 1;
    }
  }

  await ludusLearningJobRepo.update(jobId, {
    status: "done",
    fetchedCount: articles.length,
    learnedCount: learned,
    error: null,
  });
}

async function searchGuideArticles(query: string, limit: number): Promise<SearchArticle[]> {
  const q = encodeURIComponent(`${query} 攻略 OR guide OR tips OR build`);
  const url = `https://news.google.com/rss/search?q=${q}&hl=ja&gl=JP&ceid=JP:ja`;
  const xml = await fetch(url).then((r) => {
    if (!r.ok) throw new Error(`search feed failed: ${r.status}`);
    return r.text();
  });
  return parseRssItems(xml).slice(0, limit);
}

function parseRssItems(xml: string): SearchArticle[] {
  const items: SearchArticle[] = [];
  const itemBlocks = xml.match(/<item>[\s\S]*?<\/item>/g) ?? [];
  for (const block of itemBlocks) {
    const title = decodeHtml(stripCdata(extractTag(block, "title")));
    const link = stripCdata(extractTag(block, "link"));
    const desc = decodeHtml(stripCdata(extractTag(block, "description")));
    if (!title || !link) continue;
    items.push({ title, link, snippet: truncate(desc, 280) });
  }
  return items;
}

function extractMechanics(article: SearchArticle): Array<{ mechanic: string; confidence: number; tags: string[] }> {
  const text = `${article.title}\n${article.snippet}`.toLowerCase();
  const out: Array<{ mechanic: string; confidence: number; tags: string[] }> = [];
  for (const m of KNOWN_MECHANICS) {
    if (!text.includes(m)) continue;
    const confidence = scoreMechanicConfidence(text, m);
    out.push({ mechanic: m, confidence, tags: ["auto-learned", "guide-article"] });
  }
  return out;
}

function scoreMechanicConfidence(text: string, mechanic: string): number {
  const hits = text.split(mechanic).length - 1;
  if (hits >= 3) return 90;
  if (hits === 2) return 75;
  return 60;
}

function extractTag(input: string, tagName: string): string {
  const m = new RegExp(`<${tagName}>([\\s\\S]*?)<\\/${tagName}>`, "i").exec(input);
  return m?.[1]?.trim() ?? "";
}

function stripCdata(s: string): string {
  return s.replace(/^<!\[CDATA\[/, "").replace(/\]\]>$/, "").trim();
}

function decodeHtml(s: string): string {
  return s
    .replace(/&amp;/g, "&")
    .replace(/&lt;/g, "<")
    .replace(/&gt;/g, ">")
    .replace(/&quot;/g, "\"")
    .replace(/&#39;/g, "'");
}

function truncate(s: string, n: number): string {
  if (s.length <= n) return s;
  return `${s.slice(0, n)}...`;
}

