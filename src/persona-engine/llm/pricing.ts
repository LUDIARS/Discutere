/**
 * モデル別トークン単価から「等価 API コスト (USD)」を推定する。
 *
 * サブスク (claude-cli) 経路は `total_cost_usd` を envelope で受け取れるが、
 * worker-pool (transcript) / anthropic 直叩き経路は token しか得られず cost_usd が
 * NULL になる。LUDIARS 横断のコスト比較を成立させるため、token × 単価で cost_usd を
 * 補完する。単価は Anthropic の公開 list price に合わせてあり、claude-cli の
 * total_cost_usd (= 等価 API 換算の推定値) と同じ基準なので直接比較できる。
 *
 * - cost_usd は実課金ではなく **等価 API 換算の推定値** (サブスクは定額)。
 * - cache は Claude Code が打つ 5 分 ephemeral 前提: write=入力×1.25 / read=入力×0.1。
 * - 単価表に無いモデル (ローカル LLM 等) は undefined を返し、cost は NULL のまま。
 */

import type { TokenUsage } from "../types.js";

/** 1 トークンあたりの USD 単価 (= per-MTok / 1e6)。 */
interface TokenRates {
  input: number;
  output: number;
  cacheRead: number;
  cacheWrite: number;
}

const MTOK = 1_000_000;

/** per-MTok の list price から TokenRates を組む (cache は 5 分 ephemeral 前提)。 */
function rates(inputPerM: number, outputPerM: number): TokenRates {
  return {
    input: inputPerM / MTOK,
    output: outputPerM / MTOK,
    cacheRead: (inputPerM * 0.1) / MTOK, // cache read ≈ 入力の 0.1×
    cacheWrite: (inputPerM * 1.25) / MTOK, // cache write (5m TTL) ≈ 入力の 1.25×
  };
}

/**
 * モデルファミリ別単価 (Anthropic list price, USD/MTok)。
 * バージョン差 (opus-4-6/4-7/4-8 等) は同単価帯なのでファミリで丸める。
 */
const RATES_BY_FAMILY: Record<"opus" | "sonnet" | "haiku", TokenRates> = {
  opus: rates(5, 25),
  sonnet: rates(3, 15),
  haiku: rates(1, 5),
};

/** model id (例 `claude-opus-4-8`) をファミリに正規化。未知は null。 */
export function modelFamily(model: string | undefined): "opus" | "sonnet" | "haiku" | null {
  if (!model) return null;
  const m = model.toLowerCase();
  if (m.includes("opus")) return "opus";
  if (m.includes("sonnet")) return "sonnet";
  if (m.includes("haiku")) return "haiku";
  return null;
}

/**
 * usage の token と model から等価 API コスト (USD) を推定する。
 * 単価表に無いモデル、または集計対象 token が皆無なら undefined。
 */
export function estimateCostUsd(model: string | undefined, usage: TokenUsage | undefined): number | undefined {
  if (!usage) return undefined;
  const family = modelFamily(model);
  if (!family) return undefined;
  const r = RATES_BY_FAMILY[family];
  const input = usage.input_tokens ?? 0;
  const output = usage.output_tokens ?? 0;
  const cacheRead = usage.cache_read_input_tokens ?? 0;
  const cacheWrite = usage.cache_creation_input_tokens ?? 0;
  if (input === 0 && output === 0 && cacheRead === 0 && cacheWrite === 0) return undefined;
  return input * r.input + output * r.output + cacheRead * r.cacheRead + cacheWrite * r.cacheWrite;
}
