/**
 * 代理の立場判定 (統合レビュー §2.2「代理キャスト」)。
 *
 * 通常の議論は陣営を交互に割り当てるが、代理は「本人ならどちらか」を本人の発言から決める。
 * 論点 (問い) ごとに、束 (本人の主張と発言) だけを材料に 賛成 / 反対 / どちらでもない を判定 LLM に答えさせる。
 * 障害・ラベル不明は「どちらでもない」に倒す (本人と逆の陣営で戦わせるより、対立なしで終える方を選ぶ)。
 */

import type { LLMClient } from "../../persona-engine/llm/client.js";

export type ProxyStance = "pro" | "con" | "neutral";

/** 判定プロンプト (テスト用に export)。 */
export function buildProxyStancePrompt(issueTitle: string, bundleText: string): string {
  return (
    `${bundleText}\n\n` +
    `# 論点 (問い)\n${issueTitle}\n` +
    `(問いの形でなければ「${issueTitle} は成り立つか / 望ましいか」と読み替える)\n\n` +
    `上の人物の主張と発言だけを材料に、この人物がこの論点に賛成するか反対するかを判定し、ラベルを 1 つだけ返してください (説明不要):\n` +
    `- pro: 発言から賛成すると言える\n` +
    `- con: 発言から反対すると言える\n` +
    `- neutral: 発言からはどちらとも言えない / 材料が無い\n` +
    `発言に無い好みを想像で補わない。迷ったら neutral。`
  );
}

export function parseProxyStance(text: string): ProxyStance | null {
  const lower = text.toLowerCase();
  const found = (["neutral", "pro", "con"] as const)
    .map((label) => ({ label, index: lower.search(new RegExp(`\\b${label}\\b`)) }))
    .filter((x) => x.index >= 0)
    .sort((a, b) => a.index - b.index)[0];
  return found?.label ?? null;
}

export async function judgeProxyStance(args: {
  issueTitle: string;
  bundleText: string;
  /** withCostLog 済み判定 LLM (location="proxy-stance")。 */
  llm: LLMClient;
  model?: string;
  warn?: (msg: string) => void;
}): Promise<ProxyStance> {
  const { llm, model, warn = () => {} } = args;
  const result = await llm.invoke({
    prompt: buildProxyStancePrompt(args.issueTitle, args.bundleText),
    ...(model ? { model } : {}),
    maxTokens: 20,
  });
  if (!result.ok) {
    warn(`代理の立場判定 LLM エラー: ${result.error} — どちらでもないとして扱う`);
    return "neutral";
  }
  const stance = parseProxyStance(result.text);
  if (!stance) {
    warn(`代理の立場判定ラベル不明: "${result.text.slice(0, 40)}" — どちらでもないとして扱う`);
    return "neutral";
  }
  return stance;
}

export type IssueSides =
  | { opposed: true; pro: number; con: number }
  | { opposed: false; reason: string };

/**
 * 2 人の判定から論点の陣営を決める (index は参加者の並び)。
 * 賛成と反対に分かれたときだけ対立にする。同じ側・どちらでもないを無理に反対側へ回さない。
 */
export function assignIssueSides(stances: readonly ProxyStance[], labels: readonly string[]): IssueSides {
  const pro = stances.indexOf("pro");
  const con = stances.indexOf("con");
  if (pro >= 0 && con >= 0) return { opposed: true, pro, con };
  const ja = (s: ProxyStance) => (s === "pro" ? "賛成" : s === "con" ? "反対" : "どちらとも言えない");
  const detail = stances.map((s, i) => `${labels[i] ?? `代理${i + 1}`}は${ja(s)}`).join("、");
  return { opposed: false, reason: `本人の発言から見ると${detail}で、この論点では対立していません。反対役を作らずに結論なしとします。` };
}
