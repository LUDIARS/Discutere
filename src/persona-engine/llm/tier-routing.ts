/**
 * tier ルーティング (@ludiars/llm-gateway 連携)。
 *
 * 全タスクを最上位モデルで回すと高い。 facilitator の重いタスク (収束/止揚/視点追加) は
 * strong、 軽い発話生成や分類は cheap に振り分ける。 ここではタスク種別から使うモデル ID を
 * 解決するだけ (実 dispatch は LLMClient backend が行う)。
 */

import { pickTier, estimateTokens, type RoutingInput } from "@ludiars/llm-gateway";

export interface TierModels {
  cheap?: string;
  strong?: string;
}

/**
 * タスク種別・入力サイズから使うモデル ID を解決する。
 * tier 未設定 (cheap/strong とも無し) なら undefined を返し、 backend 既定モデルに委ねる
 * (= 従来の単一 model 運用と完全に同じ挙動)。
 */
export function resolveTierModel(
  input: RoutingInput,
  tiers: TierModels | undefined,
): string | undefined {
  if (!tiers || (!tiers.cheap && !tiers.strong)) return undefined;
  const tier = pickTier(input);
  return tier === "strong" ? tiers.strong ?? tiers.cheap : tiers.cheap ?? tiers.strong;
}

export { estimateTokens };
