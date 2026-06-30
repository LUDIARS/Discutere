/**
 * Anatomia ドメインカード → Di メカニクス (MechanicSummary) の精製。
 *
 * Anatomia のドメインは **コード構造観点** で抽出されるため、そのままでは議論の事前情報に
 * 向かない 2 点を LLM で精製する:
 *   1. infra / presentation / transport など **プレイヤーに見えない層を除外**する。
 *   2. description を player-facing に書き直し、`intended_affect` (狙う体験) を補う
 *      (ドメインカードの rationale は「コード境界の根拠」であって体験ではないため使わない)。
 *
 * LLM が無い / 失敗したときは **決定論フォールバック** (実装メカニクススラグを持つドメインだけ
 * 採用・description はカードのまま・intended_affect 無し) に degrade する。これは capability
 * 劣化 (LLM 不在) であって設定不備の無言フォールバックではない (§7.1)。degrade は warn に出す。
 *
 * DB / プロセスに依存しない純ロジック (単体テスト可)。
 */

import type { LLMClient } from "../../persona-engine/llm/client.js";
import type { MechanicSummary } from "../investigate.js";
import type { AnatomiaDomainCard } from "./types.js";

export interface RefineDomainsDeps {
  /** 精製に使う LLM。未指定なら決定論フォールバック。 */
  llm?: LLMClient;
  /** 議題 (LLM に「この議論の文脈で何が遊びのメカニクスか」を判断させる手掛かり)。 */
  theme?: string;
  /** 使用モデル ("" / 未指定なら LLM 既定)。 */
  model?: string;
  warn?: (msg: string) => void;
}

/** 文字列から最初の JSON 配列を取り出す (コードフェンス/前置き混入に耐える)。 */
function extractJsonArray(raw: string): unknown {
  const start = raw.indexOf("[");
  const end = raw.lastIndexOf("]");
  if (start < 0 || end <= start) return null;
  try {
    return JSON.parse(raw.slice(start, end + 1));
  } catch {
    return null;
  }
}

/**
 * 決定論フォールバック: 実装メカニクススラグを持つドメインを「遊びの要素」とみなして採用する。
 * スラグが空のドメイン (Brain/LLM Backends, Client UI, Server Protocol 等) は infra/表示層なので落とす。
 */
export function fallbackDomainsToMechanics(cards: AnatomiaDomainCard[]): MechanicSummary[] {
  return cards
    .filter((c) => c.mechanics.length > 0)
    .map((c) => {
      const slugs = c.mechanics.join(", ");
      return {
        name: c.name,
        description: slugs ? `${c.description} (実装メカニクス: ${slugs})` : c.description,
      };
    });
}

/** LLM 応答 (JSON 配列) を MechanicSummary[] に正規化する。 */
function normalize(raw: unknown): MechanicSummary[] | null {
  if (!Array.isArray(raw)) return null;
  const out: MechanicSummary[] = [];
  for (const item of raw) {
    if (!item || typeof item !== "object") continue;
    const o = item as Record<string, unknown>;
    const name = typeof o.name === "string" ? o.name.trim() : "";
    if (!name) continue;
    const description = typeof o.description === "string" ? o.description.trim() : "";
    const affect = typeof o.intended_affect === "string" ? o.intended_affect.trim() : "";
    out.push({ name, description, ...(affect ? { intended_affect: affect } : {}) });
  }
  return out;
}

function buildPrompt(cards: AnatomiaDomainCard[], theme?: string): string {
  const list = cards
    .map((c) => {
      const slugs = c.mechanics.length ? ` / 実装メカニクス: ${c.mechanics.join(", ")}` : "";
      return `- ${c.name}: ${c.description}${slugs}`;
    })
    .join("\n");
  return (
    `あなたはゲームデザインのアナリストです。以下は Anatomia がソースコード解析から抽出した` +
    `あるゲームの「ドメイン (実装上の責務の塊)」一覧です。\n\n` +
    `${list}\n\n` +
    (theme ? `議論テーマ: ${theme}\n\n` : "") +
    `この一覧から、**プレイヤーが体験する「遊びのメカニクス」** だけを抽出してください。\n` +
    `- インフラ層 (LLM backend, サーバ/通信プロトコル, データ配置)・表示層 (UI/レンダリング)・` +
    `内部基盤は **除外** する。\n` +
    `- 採用する各メカニクスは、description を **プレイヤー視点** で言い直し、` +
    `intended_affect に「そのメカニクスが狙う体験/感情」を 1 文で書く。\n` +
    `- 実装用語ではなく、遊びとして何が起きるかを書く。\n\n` +
    `出力は JSON 配列のみ (前置き/コードフェンス無し):\n` +
    `[{"name":string,"description":string,"intended_affect":string}]`
  );
}

/**
 * ドメインカードを Di の事前情報メカニクスに精製する。
 * LLM 採用ゼロ件 / 失敗 / 未指定なら決定論フォールバックに degrade する。
 */
export async function refineDomainsToMechanics(
  cards: AnatomiaDomainCard[],
  deps: RefineDomainsDeps = {}
): Promise<MechanicSummary[]> {
  const warn = deps.warn ?? (() => {});
  if (cards.length === 0) return [];

  if (!deps.llm) {
    return fallbackDomainsToMechanics(cards);
  }

  const result = await deps.llm.invoke({
    prompt: buildPrompt(cards, deps.theme),
    model: deps.model || undefined,
  });
  if (!result.ok) {
    warn(`Anatomia 精製 LLM 失敗 (${result.error}) → 決定論フォールバック`);
    return fallbackDomainsToMechanics(cards);
  }
  const mechanics = normalize(extractJsonArray(result.text));
  if (!mechanics || mechanics.length === 0) {
    warn("Anatomia 精製 LLM 応答が空/不正 → 決定論フォールバック");
    return fallbackDomainsToMechanics(cards);
  }
  return mechanics;
}
