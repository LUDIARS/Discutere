/**
 * LLM 呼び出し Vestigium ロガー (Discutere)。
 *
 * VESTIGIUM_LOGS_DIR (Excubitor が注入) または cwd/logs に
 * channel='llm' の JSONL を書く。失敗は無視 (サービス本体を落とさない)。
 * プロンプトをそのまま ctx に含める (ユーザ指示)。
 * spec 正本: LUDIARS/Vestigium/DESIGN.md §2.2
 */

import fs from 'node:fs';
import path from 'node:path';

const SERVICE_CODE = 'discutere';

function logsRoot(): string {
  return process.env.VESTIGIUM_LOGS_DIR ?? path.join(process.cwd(), 'logs');
}

function ymdUtc(d: Date): string {
  return d.toISOString().slice(0, 10);
}

let _stream: fs.WriteStream | null = null;
let _streamYmd = '';

function getStream(): fs.WriteStream {
  const now = new Date();
  const ymd = ymdUtc(now);
  if (_stream && _streamYmd === ymd) return _stream;
  try { _stream?.end(); } catch { /* noop */ }
  const dir = path.join(logsRoot(), SERVICE_CODE);
  fs.mkdirSync(dir, { recursive: true });
  _stream = fs.createWriteStream(path.join(dir, `${ymd}.jsonl`), { flags: 'a', encoding: 'utf8' });
  _streamYmd = ymd;
  return _stream;
}

export interface LlmLogArgs {
  backend: string;
  model: string;
  kind?: string;
  system?: string;
  prompt: string;
  input_tokens?: number;
  output_tokens?: number;
  cache_read_tokens?: number;
  cache_creation_tokens?: number;
  cost_usd?: number;
  ok: boolean;
  error?: string;
}

export function logLlm(args: LlmLogArgs): void {
  try {
    const ctx: Record<string, unknown> = {
      backend: args.backend,
      model: args.model,
      prompt_chars: args.prompt.length,
      prompt: args.prompt,
      ok: args.ok,
    };
    if (args.kind !== undefined) ctx.kind = args.kind;
    if (args.system !== undefined) { ctx.system_chars = args.system.length; ctx.system = args.system; }
    if (args.input_tokens !== undefined) ctx.input_tokens = args.input_tokens;
    if (args.output_tokens !== undefined) ctx.output_tokens = args.output_tokens;
    if (args.cache_read_tokens !== undefined) ctx.cache_read_tokens = args.cache_read_tokens;
    if (args.cache_creation_tokens !== undefined) ctx.cache_creation_tokens = args.cache_creation_tokens;
    if (args.cost_usd !== undefined) ctx.cost_usd = args.cost_usd;
    if (args.error !== undefined) ctx.error = args.error;

    const rec = JSON.stringify({
      ts: Date.now(),
      level: args.ok ? 'info' : 'warn',
      service: SERVICE_CODE,
      channel: 'llm',
      msg: `[llm] ${args.backend} ${args.model}`,
      pid: process.pid,
      ctx,
    }) + '\n';
    getStream().write(rec);
  } catch {
    // failure-safe
  }
}
