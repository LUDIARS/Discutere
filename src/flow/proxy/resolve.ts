/**
 * 代理議論の起動条件の解決 (dispatch から呼ぶ、S3)。
 *
 * 観点タグ「代理」が付いた議論/改善だけが対象。起動できない理由はすべて ProxyRequestError で返し、
 * 黙って通常の議論に切り替えない (人は代理議論を頼んでいるため)。
 *   - flow.proxy.enabled=false
 *   - 機密 / 内部 タグ (外部の声と同じく、公開レビュー由来の人物像も使わない)
 *   - 議題に「代理: 論者#a, 論者#b」が無い / 人物像が使えない
 */

import { getConfig } from "../../config.js";
import { extractKeyTerms } from "../../discatier-engine-adapter/keyword-terms.js";
import { canCollectExternal, wantsProxy, type FlowTag } from "../tags.js";
import type { FlowPersona } from "../personas.js";
import { loadProxyParticipants, parseProxyRequest, ProxyRequestError } from "./cast.js";

export interface ProxyRun {
  /** 指定部分を除いた議題。 */
  theme: string;
  participants: FlowPersona[];
}

/** 代理議論を求めていなければ null。求めているのに起動できなければ ProxyRequestError。 */
export function resolveProxyRun(theme: string, tags: readonly FlowTag[], now: number = Date.now()): ProxyRun | null {
  if (!wantsProxy(tags)) return null;
  const cfg = getConfig();
  if (!cfg.flow.proxy.enabled) {
    throw new ProxyRequestError("代理議論は無効になっています (flow.proxy.enabled=false)。");
  }
  if (!canCollectExternal(tags)) {
    throw new ProxyRequestError("機密 / 内部の議論では、公開レビューから作った代理は使えません。");
  }
  const request = parseProxyRequest(theme);
  if (!request) {
    throw new ProxyRequestError("議題に「代理: 論者#xxxxxx, 論者#yyyyyy」を書いてください。");
  }
  const defaultModel = cfg.llm.model ?? "claude-haiku-4-5-20251001";
  const discussants = cfg.flow.roster.discussants.length > 0 ? cfg.flow.roster.discussants : [defaultModel];
  const participants = loadProxyParticipants({
    request,
    now,
    requirePassed: cfg.flow.proxy.requireEvalPassed,
    budgetTokens: cfg.flow.proxy.budgetTokens,
    models: discussants.slice(0, 2),
    isLocal: cfg.llm.backend === "local",
    focusTerms: extractKeyTerms([request.theme]),
  });
  return { theme: request.theme, participants };
}
