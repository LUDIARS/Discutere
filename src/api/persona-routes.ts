/**
 * Persona generation/admin UI.
 *
 * The C1/C2 persona generation pipeline already exists as CLI scripts. This
 * route exposes the same operations from the loopback WebUI.
 */

import { existsSync, readFileSync, readdirSync, writeFileSync } from "node:fs";
import path from "node:path";
import { Hono } from "hono";

import { getConfig, _resetConfig } from "../config.js";
import { FallbackLlm } from "../flow/llm-fallback.js";
import { estimatePopulations, persistPopulations } from "../flow/persona-populations.js";
import { generateSyntheticPersonas, type SurveyGame } from "../flow/persona-survey.js";
import {
  buildPersonaQuestionnaire,
  createPersonaFromQuestionnaireAnswers,
  type PersonaQuestionnaire,
} from "../flow/persona-questionnaire.js";
import { runAdoptFromKg } from "../flow/persona-adopt-runner.js";
import { AnthropicSdkClient } from "../persona-engine/llm/anthropic.js";
import { ClaudeCliClient } from "../persona-engine/llm/claude-cli.js";
import { readClaudeCodeToken } from "../persona-engine/llm/claude-code-auth.js";

const personaRoutes = new Hono();

function configPath(): string {
  return path.resolve(process.env.DISCUTERE_CONFIG ?? "./discutere.config.json");
}

function readConfigFile(): Record<string, unknown> {
  const file = configPath();
  if (!existsSync(file)) return {};
  return JSON.parse(readFileSync(file, "utf8")) as Record<string, unknown>;
}

function writeFlowConfig(patch: Record<string, unknown>): void {
  const current = readConfigFile();
  const flow = (current.flow && typeof current.flow === "object" ? current.flow : {}) as Record<string, unknown>;
  current.flow = { ...flow, ...patch };
  writeFileSync(configPath(), JSON.stringify(current, null, 2) + "\n", "utf8");
  _resetConfig();
}

function loadGames(): SurveyGame[] {
  const dir = path.resolve("data/games");
  if (!existsSync(dir)) return [];
  return readdirSync(dir)
    .filter((f) => f.endsWith(".md"))
    .map((f) => {
      const slug = f.replace(/\.md$/, "");
      return { slug, title: slug };
    });
}

function createPersonaLlm() {
  const cfg = getConfig();
  return new FallbackLlm(
    new AnthropicSdkClient({
      getAuthToken: () => readClaudeCodeToken(),
      apiKey: cfg.llm.anthropicApiKey || undefined,
      defaultModel: cfg.llm.model || undefined,
    }),
    new ClaudeCliClient({
      defaultTimeoutMs: cfg.llm.claudeCliTimeoutMs,
      defaultModel: cfg.llm.model || undefined,
      gitBashPath: cfg.workerPool.gitBashPath ?? cfg.llm.gitBashPath,
    })
  );
}

function boolValue(v: unknown, fallback: boolean): boolean {
  if (typeof v === "boolean") return v;
  if (typeof v === "string") return /^(1|true|yes|on)$/i.test(v.trim());
  return fallback;
}

function numValue(v: unknown, fallback: number, min: number, max: number): number {
  const n = typeof v === "number" ? v : Number(v);
  if (!Number.isFinite(n)) return fallback;
  return Math.max(min, Math.min(max, Math.floor(n)));
}

function stringList(v: unknown): string[] {
  if (Array.isArray(v)) return v.map((item) => (typeof item === "string" ? item.trim() : "")).filter(Boolean);
  if (typeof v === "string") return v.split(/\r?\n|,/).map((s) => s.trim()).filter(Boolean);
  return [];
}

personaRoutes.get("/admin/personas", (c) => c.html(PERSONA_HTML));

personaRoutes.get("/admin/personas/data", (c) => {
  const cfg = getConfig();
  return c.json({
    autoAdoptOnCrawl: cfg.flow.autoAdoptOnCrawl,
    envOverride: process.env.DISCUTERE_FLOW_AUTO_ADOPT_ON_CRAWL ?? null,
    configPath: configPath(),
    games: loadGames().length,
  });
});

personaRoutes.put("/admin/personas/auto-adopt", async (c) => {
  const body = (await c.req.json().catch(() => ({}))) as { enabled?: unknown };
  if (body.enabled === undefined) return c.json({ error: "enabled required" }, 400);
  try {
    writeFlowConfig({ autoAdoptOnCrawl: boolValue(body.enabled, false) });
  } catch (err) {
    return c.json({ error: `config write failed: ${(err as Error).message}` }, 500);
  }
  return c.json({
    ok: true,
    autoAdoptOnCrawl: getConfig().flow.autoAdoptOnCrawl,
    envOverride: process.env.DISCUTERE_FLOW_AUTO_ADOPT_ON_CRAWL ?? null,
    configPath: configPath(),
  });
});

personaRoutes.post("/admin/personas/adopt", async (c) => {
  const body = (await c.req.json().catch(() => ({}))) as {
    source?: unknown;
    minOpinions?: unknown;
    dry?: unknown;
  };
  const sourceFilter = typeof body.source === "string" && body.source.trim() ? body.source.trim() : undefined;
  const minOpinions = numValue(body.minOpinions, 10, 1, 10_000);
  const dry = boolValue(body.dry, true);
  try {
    const summary = runAdoptFromKg({ sourceFilter, minOpinions, dry });
    return c.json({ ok: true, dry, sourceFilter: sourceFilter ?? null, minOpinions, ...summary });
  } catch (err) {
    return c.json({ error: (err as Error).message }, 500);
  }
});

personaRoutes.post("/admin/personas/survey", async (c) => {
  const body = (await c.req.json().catch(() => ({}))) as {
    count?: unknown;
    gamesPerIndividual?: unknown;
  };
  const games = loadGames();
  if (games.length === 0) return c.json({ error: "data/games/*.md not found" }, 400);
  const count = numValue(body.count, 1, 1, 100);
  const gamesPerIndividual =
    body.gamesPerIndividual === undefined ? undefined : numValue(body.gamesPerIndividual, 8, 1, 100);
  try {
    const result = await generateSyntheticPersonas({ count, games, gamesPerIndividual, llm: createPersonaLlm() });
    const totalPlayed = result.breakdown.reduce((sum, b) => sum + b.played, 0);
    const totalUnplayed = result.breakdown.reduce((sum, b) => sum + b.unplayed, 0);
    return c.json({
      ok: true,
      requested: count,
      saved: result.personas.length,
      totalPlayed,
      totalUnplayed,
      personas: result.personas.map((p) => ({ id: p.id, name: p.name, label: p.label })),
    });
  } catch (err) {
    return c.json({ error: (err as Error).message }, 500);
  }
});

personaRoutes.post("/admin/personas/questionnaire", async (c) => {
  const body = (await c.req.json().catch(() => ({}))) as {
    gameTitle?: unknown;
    gameSlug?: unknown;
    mechanicsContext?: unknown;
    userVoices?: unknown;
    questionCount?: unknown;
    genericOnly?: unknown;
  };
  const gameTitle = typeof body.gameTitle === "string" ? body.gameTitle.trim() : "";
  if (!gameTitle) return c.json({ error: "gameTitle required" }, 400);
  const genericOnly = boolValue(body.genericOnly, false);
  try {
    const questionnaire = await buildPersonaQuestionnaire({
      gameTitle,
      gameSlug: typeof body.gameSlug === "string" && body.gameSlug.trim() ? body.gameSlug.trim() : undefined,
      mechanicsContext:
        typeof body.mechanicsContext === "string" && body.mechanicsContext.trim()
          ? body.mechanicsContext.trim()
          : undefined,
      userVoices: stringList(body.userVoices),
      questionCount: numValue(body.questionCount, 9, 3, 24),
      llm: genericOnly ? undefined : createPersonaLlm(),
    });
    return c.json({ ok: true, questionnaire });
  } catch (err) {
    return c.json({ error: (err as Error).message }, 500);
  }
});

personaRoutes.post("/admin/personas/questionnaire/answer", async (c) => {
  const body = (await c.req.json().catch(() => ({}))) as {
    questionnaire?: unknown;
    answers?: unknown;
    name?: unknown;
    role?: unknown;
  };
  const questionnaire =
    body.questionnaire && typeof body.questionnaire === "object"
      ? (body.questionnaire as PersonaQuestionnaire)
      : null;
  if (!questionnaire) return c.json({ error: "questionnaire required" }, 400);
  const answers =
    body.answers && typeof body.answers === "object" ? (body.answers as Record<string, unknown>) : {};
  try {
    const result = await createPersonaFromQuestionnaireAnswers({
      questionnaire,
      answers,
      name: typeof body.name === "string" && body.name.trim() ? body.name.trim() : undefined,
      role:
        body.role === "opinion" || body.role === "debater" || body.role === "facilitator"
          ? body.role
          : "opinion",
      llm: createPersonaLlm(),
    });
    return c.json({
      ok: true,
      saved: result.saved,
      persona: {
        id: result.persona.id,
        name: result.persona.name,
        role: result.persona.role,
        label: result.persona.label,
        traits: result.persona.traits,
      },
      analysis: {
        responseVector: result.analysis.responseVector,
        preferenceVector: result.analysis.preferenceVector,
        preferenceScores: result.analysis.preferenceScores,
        topPreferenceAxes: result.analysis.topPreferenceAxes,
        topPositiveDeltas: result.analysis.topPositiveDeltas,
        topNegativeDeltas: result.analysis.topNegativeDeltas,
      },
    });
  } catch (err) {
    return c.json({ error: (err as Error).message }, 500);
  }
});

personaRoutes.post("/admin/personas/populations", async (c) => {
  const body = (await c.req.json().catch(() => ({}))) as {
    threshold?: unknown;
    bigRatio?: unknown;
    realOrigins?: unknown;
    persist?: unknown;
  };
  const threshold = Number(body.threshold ?? 0.9);
  const bigRatio = Number(body.bigRatio ?? 0.15);
  const realOrigins =
    typeof body.realOrigins === "string" && body.realOrigins.trim()
      ? body.realOrigins.split(",").map((s) => s.trim()).filter(Boolean)
      : undefined;
  const persist = boolValue(body.persist, true);
  try {
    const report = estimatePopulations({
      simThreshold: Number.isFinite(threshold) ? threshold : 0.9,
      bigRatio: Number.isFinite(bigRatio) ? bigRatio : 0.15,
      realOrigins: realOrigins as never,
    });
    const written = persist && report.syntheticCount > 0 ? persistPopulations(report, Date.now()) : 0;
    return c.json({ ok: true, persist, written, ...report });
  } catch (err) {
    return c.json({ error: (err as Error).message }, 500);
  }
});

const PERSONA_HTML = `<!doctype html>
<html lang="ja"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><meta name="color-scheme" content="light dark">
<title>Discutere ペルソナ生成</title>
<style>
 :root{color-scheme:light;--bg:#f8fafc;--fg:#111827;--muted:#64748b;--surface:#fff;--field:#fff;--border:#e2e8f0;--border-strong:#cbd5e1;--primary:#2563eb;--secondary:#e2e8f0;--secondary-fg:#334155;--ok:#16a34a;--bad:#dc2626;--link:#2563eb}
 @media (prefers-color-scheme:dark){:root{color-scheme:dark;--bg:#0f1115;--fg:#e6e6e6;--muted:#8b94a3;--surface:#161922;--field:#0c0e13;--border:#232733;--border-strong:#2a2f3a;--primary:#2563eb;--secondary:#2a2f3a;--secondary-fg:#cbd3e1;--ok:#22c55e;--bad:#f87171;--link:#7aa2ff}}
 body{font-family:system-ui,'Segoe UI',sans-serif;margin:0;background:var(--bg);color:var(--fg)}
 .wrap{max-width:920px;margin:0 auto;padding:24px}
 a{color:var(--link)} h1{font-size:20px;margin:0 0 6px} h2{font-size:15px;margin:22px 0 8px;color:var(--fg)}
 .sub,.muted{color:var(--muted);font-size:13px}.card{background:var(--surface);border:1px solid var(--border);border-radius:8px;padding:14px 16px;margin-bottom:14px}
 label{display:block;font-size:12px;color:var(--muted);margin:8px 0 3px}
 input,textarea,select{width:100%;box-sizing:border-box;background:var(--field);color:var(--fg);border:1px solid var(--border-strong);border-radius:7px;padding:8px 10px;font-size:13px}
 textarea{min-height:80px;resize:vertical}
 .row{display:flex;gap:12px;flex-wrap:wrap}.row>div{flex:1;min-width:150px}
 .qitem{border-top:1px solid var(--border);padding-top:10px;margin-top:10px}
 button{border:0;border-radius:8px;padding:8px 15px;font-size:13px;cursor:pointer;background:var(--primary);color:#fff;margin-top:10px}
 button.ghost{background:var(--secondary);color:var(--secondary-fg)}.ok{color:var(--ok)}.bad{color:var(--bad)}
 pre{white-space:pre-wrap;background:var(--field);border:1px solid var(--border);border-radius:8px;padding:10px;max-height:300px;overflow:auto}
 .switch{display:flex;align-items:center;gap:10px}.switch input{width:auto}
</style></head><body><div class="wrap">
 <p><a href="/">トップ</a></p>
 <h1>ペルソナ生成</h1>
 <p class="sub">C1 実在ユーザ採用、C2 合成生成、母数推定を試験できます。</p>
 <div class="card">
   <h2>クロール後の自動採用</h2>
   <label class="switch"><input id="auto" type="checkbox"> flow.autoAdoptOnCrawl を有効にする</label>
   <p class="muted" id="autoMeta"></p>
   <button onclick="saveAuto()">設定を保存</button> <span id="autoMsg"></span>
 </div>
 <div class="card">
   <h2>C1 実在ユーザ採用</h2>
   <div class="row"><div><label>source</label><input id="source" placeholder="youtube / steam / 空なら全体"></div><div><label>min opinions</label><input id="minOpinions" type="number" value="10" min="1"></div></div>
   <label class="switch"><input id="dry" type="checkbox" checked> dry-run</label>
   <button onclick="runAdopt()">採用を試す</button>
 </div>
 <div class="card">
   <h2>C2 合成生成</h2>
   <div class="row"><div><label>count</label><input id="surveyCount" type="number" value="1" min="1" max="100"></div><div><label>games per individual</label><input id="gamesPer" type="number" placeholder="既定"></div></div>
   <button onclick="runSurvey()">合成生成を実行</button>
 </div>
 <div class="card">
   <h2>回答型ペルソナ生成</h2>
   <p class="muted">個人の嗜好/考え方に答える質問票をLLMで構築し、回答をゲーム基準ベクトルとの差分として解析してペルソナ保存します。</p>
   <div class="row"><div><label>game title</label><input id="qGameTitle" placeholder="モンスターストライク"></div><div><label>question count</label><input id="qCount" type="number" value="9" min="3" max="24"></div></div>
   <label>基本情報/メカニクス</label><textarea id="qMechanics" placeholder="ゲームの基本ループ、チュートリアル、報酬、育成、協力要素など"></textarea>
   <label>ユーザーの声 (1行1件)</label><textarea id="qVoices" placeholder="外部の声や学習済みの代表的な反応"></textarea>
   <button onclick="buildQuestionnaire()">質問票を構築</button>
   <button class="ghost" onclick="buildGenericQuestionnaire()">汎用質問集を読み込む</button>
   <div id="qForm"></div>
   <div class="row"><div><label>persona name (optional)</label><input id="qPersonaName" placeholder="未指定なら自動"></div><div><label>role</label><select id="qRole"><option value="opinion">opinion</option><option value="debater">debater</option><option value="facilitator">facilitator</option></select></div></div>
   <button onclick="saveQuestionnairePersona()">回答からペルソナ保存</button>
 </div>
 <div class="card">
   <h2>C2-b 母数推定</h2>
   <div class="row"><div><label>threshold</label><input id="threshold" value="0.9"></div><div><label>big ratio</label><input id="bigRatio" value="0.15"></div><div><label>real origins</label><input id="realOrigins" value="adopted"></div></div>
   <label class="switch"><input id="persist" type="checkbox" checked> 結果を書き戻す</label>
   <button onclick="runPop()">母数推定を実行</button>
 </div>
 <pre id="out"></pre>
</div><script>
const $=id=>document.getElementById(id);
let currentQuestionnaire=null;
function show(x){ $('out').textContent=typeof x==='string'?x:JSON.stringify(x,null,2); }
async function load(){
 const j=await fetch('/api/admin/personas/data').then(r=>r.json());
 $('auto').checked=!!j.autoAdoptOnCrawl;
 $('autoMeta').textContent='config: '+j.configPath+' / games: '+j.games+(j.envOverride!==null?' / env override: '+j.envOverride:'');
}
async function post(url,body){ const r=await fetch(url,{method:'POST',headers:{'content-type':'application/json'},body:JSON.stringify(body)}); const j=await r.json(); show(j); return j; }
async function saveAuto(){
 const r=await fetch('/api/admin/personas/auto-adopt',{method:'PUT',headers:{'content-type':'application/json'},body:JSON.stringify({enabled:$('auto').checked})});
 const j=await r.json(); $('autoMsg').className=j.ok?'ok':'bad'; $('autoMsg').textContent=j.ok?'保存しました':'失敗'; show(j); load();
}
async function runAdopt(){ await post('/api/admin/personas/adopt',{source:$('source').value,minOpinions:Number($('minOpinions').value),dry:$('dry').checked}); }
async function runSurvey(){ await post('/api/admin/personas/survey',{count:Number($('surveyCount').value),gamesPerIndividual:$('gamesPer').value?Number($('gamesPer').value):undefined}); }
function renderQuestionnaire(q){
 currentQuestionnaire=q;
 const html=q.questions.map((item,i)=>{
   const id='qa_'+item.id;
   const opts=(item.options||[]).map(o=>'<option value="'+escapeHtml(o)+'">'+escapeHtml(o)+'</option>').join('');
   const field=opts
     ? '<select id="'+id+'"><option value="">選択</option>'+opts+'</select>'
     : '<textarea id="'+id+'" placeholder="回答を入力"></textarea>';
   return '<div class="qitem"><div class="muted">'+(i+1)+'. '+escapeHtml(item.kind)+' / '+escapeHtml(item.metric)+' / weight '+item.weight+'</div><label>'+escapeHtml(item.question)+'</label>'+field+'</div>';
 }).join('');
 $('qForm').innerHTML=html;
}
function escapeHtml(s){ return String(s).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c])); }
async function buildQuestionnaire(){
 const gameTitle=$('qGameTitle').value.trim();
 if(!gameTitle){ alert('game title required'); return; }
 const j=await post('/api/admin/personas/questionnaire',{gameTitle,mechanicsContext:$('qMechanics').value,userVoices:$('qVoices').value,questionCount:Number($('qCount').value)});
 if(j.ok) renderQuestionnaire(j.questionnaire);
}
async function buildGenericQuestionnaire(){
 const gameTitle=$('qGameTitle').value.trim()||'対象ゲーム';
 const j=await post('/api/admin/personas/questionnaire',{gameTitle,mechanicsContext:$('qMechanics').value,userVoices:$('qVoices').value,questionCount:Number($('qCount').value),genericOnly:true});
 if(j.ok) renderQuestionnaire(j.questionnaire);
}
function collectQuestionnaireAnswers(){
 const out={};
 if(!currentQuestionnaire) return out;
 for(const q of currentQuestionnaire.questions){
   const el=$('qa_'+q.id);
   if(el && el.value.trim()) out[q.id]=el.value.trim();
 }
 return out;
}
async function saveQuestionnairePersona(){
 if(!currentQuestionnaire){ alert('先に質問票を構築してください'); return; }
 await post('/api/admin/personas/questionnaire/answer',{questionnaire:currentQuestionnaire,answers:collectQuestionnaireAnswers(),name:$('qPersonaName').value,role:$('qRole').value});
}
async function runPop(){ await post('/api/admin/personas/populations',{threshold:Number($('threshold').value),bigRatio:Number($('bigRatio').value),realOrigins:$('realOrigins').value,persist:$('persist').checked}); }
load();
</script></body></html>`;

export { personaRoutes };
