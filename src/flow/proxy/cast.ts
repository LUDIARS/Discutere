/**
 * 代理議論のキャスト (統合レビュー §2.2、S3)。
 *
 * - 議題文から「代理: 論者#aaaaaa, 論者#bbbbbb」(と任意の「ジャンル: xxx」) を読み取る
 * - 取り込み済みの人物パックから 2 人を引き、キャストに出してよいか (失効・期限・材料・評価) を確かめる
 * - 束を作って FlowPersona (debater、立場は論点ごとに driver が決める) にする
 * 欠員を架空の人物で埋めない。足りなければ理由付きで ProxyRequestError を投げる。
 */

import type { FlowPersona } from "../personas.js";
import { renderProxyBundle } from "./bundle.js";
import { findProxyProfilesByLabel, isCastable, type ProxyProfileRow } from "./store.js";

/** 代理議論で露出名の括弧内に出す区分。 */
export const PROXY_POSSESSION_LABEL = "AI代理";

export class ProxyRequestError extends Error {
  constructor(message: string) {
    super(message);
    this.name = "ProxyRequestError";
  }
}

export interface ProxyRequest {
  labels: string[];
  genre: string | null;
  /** 指定部分を取り除いた議題。 */
  theme: string;
}

const LABEL = /論者#[0-9a-f]{6}/g;
const DIRECTIVE = /代理\s*[:：]\s*((?:論者#[0-9a-f]{6}[\s,、と&vsVS]*)+)/;
const GENRE = /ジャンル\s*[:：]\s*([a-z0-9][a-z0-9_-]*)/;

/** 議題文から代理の指定を読み取る。指定が無ければ null。 */
export function parseProxyRequest(text: string): ProxyRequest | null {
  const m = text.match(DIRECTIVE);
  if (!m) return null;
  const labels = [...new Set(m[1].match(LABEL) ?? [])];
  const genreMatch = text.match(GENRE);
  const theme = text
    .replace(m[0], "")
    .replace(genreMatch?.[0] ?? "", "")
    .replace(/^[\s/|・,、]+|[\s/|・,、]+$/g, "")
    .trim();
  return { labels, genre: genreMatch?.[1] ?? null, theme: theme || text.trim() };
}

/** 2 人に共通するジャンルを 1 つに決める (指定があればそれ)。 */
export function chooseGenre(rowsByLabel: readonly ProxyProfileRow[][], requested: string | null): string {
  if (requested) return requested;
  const sets = rowsByLabel.map((rows) => new Set(rows.map((r) => r.genre)));
  const common = [...(sets[0] ?? [])].filter((g) => sets.every((s) => s.has(g)));
  if (common.length === 1) return common[0];
  if (common.length === 0) throw new ProxyRequestError("2 人に共通するジャンルの人物像がありません。「ジャンル: <id>」で指定してください。");
  throw new ProxyRequestError(`共通するジャンルが複数あります (${common.join(", ")})。「ジャンル: <id>」で指定してください。`);
}

export interface LoadProxyParticipantsArgs {
  request: ProxyRequest;
  now: number;
  /** 本人らしさの評価に合格した人物だけを出すか (flow.proxy.requireEvalPassed)。 */
  requirePassed: boolean;
  budgetTokens: number;
  /** 参加者ごとのモデル (足りなければ最後のものを使い回す)。 */
  models: readonly string[];
  isLocal: boolean;
  focusTerms: readonly string[];
}

/** 代理 2 人を FlowPersona にする。 */
export function loadProxyParticipants(args: LoadProxyParticipantsArgs): FlowPersona[] {
  const { request } = args;
  if (request.labels.length !== 2) {
    throw new ProxyRequestError(`代理議論は 2 人で行います (指定 ${request.labels.length} 人)。「代理: 論者#xxxxxx, 論者#yyyyyy」と書いてください。`);
  }
  const rowsByLabel = request.labels.map((label) => {
    const rows = findProxyProfilesByLabel(label);
    if (rows.length === 0) throw new ProxyRequestError(`${label} の人物像が取り込まれていません。`);
    return rows;
  });
  const genre = chooseGenre(rowsByLabel, request.genre);

  return request.labels.map((label, i) => {
    const row = rowsByLabel[i].find((r) => r.genre === genre);
    if (!row) throw new ProxyRequestError(`${label} にジャンル ${genre} の人物像がありません。`);
    const blocked = isCastable(row, args.now, args.requirePassed);
    if (blocked) throw new ProxyRequestError(`${label} (${genre}) は代理に出せません: ${blocked}。`);
    const bundle = renderProxyBundle(row.pack, { budgetTokens: args.budgetTokens, focusTerms: args.focusTerms, label });
    if (!bundle) throw new ProxyRequestError(`${label} (${genre}) の人物像を予算 ${args.budgetTokens} トークンに収められません。`);
    const model = args.models[i] ?? args.models[args.models.length - 1];
    if (!model) throw new ProxyRequestError("代理に割り当てるモデルがありません。");
    const persona: FlowPersona = {
      id: `proxy:${row.speakerKey}:${genre}`,
      name: label,
      role: "debater",
      stance: "pro",
      speechStyle: row.pack.styleNotes.length > 0 ? row.pack.styleNotes.join(" / ") : "本人の発言に近い口語",
      traits: ["公開レビュー由来の AI 代理", `ジャンル ${genre}`],
      model,
      isLocal: args.isLocal,
      possession: { label: PROXY_POSSESSION_LABEL },
      proxy: {
        label,
        speakerKey: row.speakerKey,
        genre,
        bundleText: bundle.text,
        evidenceIds: bundle.evidenceIds,
        evidenceTexts: bundle.evidenceTexts,
      },
    };
    return persona;
  });
}
