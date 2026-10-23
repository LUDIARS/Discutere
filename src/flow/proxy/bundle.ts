/**
 * 人物パック → 代理の prompt に載せる「束」(統合レビュー §2.2、S2)。
 *
 * - 数値ベクトルは載せない。20D は望む / 不満の次元名を日本語にした説明文にだけ使う
 * - 本人の発言は [E1] のように id 付きで載せ、代理はこの id でしか本人の根拠を引けない
 * - 予算 (トークン概算) を超えたら、議題と関係の薄い発言から落とす。落とした発言だけを根拠にした
 *   立場カードも一緒に落とす。発言が 1 件も載らない予算なら束を作らない (null)
 * - confidence=low のパックは束にしない
 *
 * 純関数 (DB / LLM なし)。
 */

import type { PackEvidence, PersonaPack, StanceCard } from "./contract.js";

/** 日本語混じりの文のトークン概算 (1 文字 ≈ 1.3 トークン。予算は安全側に倒す)。 */
export function estimateTokens(text: string): number {
  return Math.ceil(text.length * 1.3);
}

const DIM_LABELS: Record<string, string> = {
  "emo.valence": "全体の好意",
  "emo.arousal": "熱量",
  "emo.joy": "楽しさ・喜び",
  "emo.trust": "信頼",
  "emo.fear": "不安",
  "emo.surprise": "驚き",
  "emo.sadness": "寂しさ",
  "emo.disgust": "嫌悪",
  "emo.anger": "怒り",
  "emo.anticipation": "期待",
  "asp.fun": "遊びの面白さ",
  "asp.difficulty": "難易度",
  "asp.content": "遊べる量",
  "asp.price_value": "値段に見合うか",
  "asp.performance": "動作の快適さ",
  "asp.story": "物語",
  "asp.graphics": "見た目",
  "asp.replayability": "繰り返し遊べるか",
};

export function dimLabel(dim: string): string {
  return DIM_LABELS[dim] ?? dim;
}

export interface ProxyBundle {
  /** prompt に載せる本文。 */
  text: string;
  /** 本文に載った根拠 id (代理が引いてよい id)。 */
  evidenceIds: string[];
  /** 載った根拠の本文 (引用の検査に使う)。 */
  evidenceTexts: Record<string, string>;
  estimatedTokens: number;
  /** 予算のために落とした根拠の数。 */
  droppedEvidence: number;
}

export interface BundleOptions {
  /** 束の予算 (トークン概算)。 */
  budgetTokens: number;
  /** 議題の語。関係の深い発言を先に残す。 */
  focusTerms?: readonly string[];
  /** 露出名 (論者#xxxxxx)。 */
  label: string;
}

function relevance(e: PackEvidence, terms: readonly string[]): number {
  let score = 0;
  for (const t of terms) if (t && (e.text.includes(t) || e.gameTitle.includes(t))) score++;
  return score;
}

/** 議題に近い順。同点は正負を交互に (片方の極性だけが残らないように)、最後は id 順。 */
export function orderEvidence(evidence: readonly PackEvidence[], terms: readonly string[]): PackEvidence[] {
  const scored = evidence.map((e, i) => ({ e, i, s: relevance(e, terms) }));
  const pos = scored.filter((x) => x.e.polarity === "pos");
  const neg = scored.filter((x) => x.e.polarity === "neg");
  const byScore = (a: (typeof scored)[number], b: (typeof scored)[number]) => b.s - a.s || a.i - b.i;
  pos.sort(byScore);
  neg.sort(byScore);
  const interleaved: (typeof scored)[number][] = [];
  for (let k = 0; k < Math.max(pos.length, neg.length); k++) {
    if (pos[k]) interleaved.push(pos[k]);
    if (neg[k]) interleaved.push(neg[k]);
  }
  // 議題との関係が強いものを先頭へ (安定ソート: 同点は交互の並びを保つ)。
  return interleaved
    .map((x, order) => ({ ...x, order }))
    .sort((a, b) => b.s - a.s || a.order - b.order)
    .map((x) => x.e);
}

function header(pack: PersonaPack, label: string): string[] {
  const lines = [
    `# あなたが代理する人物: ${label}`,
    `この人物は公開レビューの発言から作った AI の代理です。本人ではなく、本人の発言・同意でもありません。`,
    `ジャンル: ${pack.genre} / 材料: 発言 ${pack.sufficiency.utterances} 件 (好意的 ${pack.sufficiency.positive}・否定的 ${pack.sufficiency.negative})、ゲーム ${pack.sufficiency.games} 本`,
  ];
  const wants = pack.designGap.wants.map(dimLabel);
  const complaints = pack.designGap.complaints.map(dimLabel);
  if (wants.length > 0) lines.push(`このジャンルで求めているもの: ${wants.join("・")}`);
  if (complaints.length > 0) lines.push(`このジャンルで不満を持ちやすいもの: ${complaints.join("・")}`);
  for (const g of pack.conditionalGaps) {
    lines.push(`望み: ${g.current} → ${g.desired}${g.condition ? ` (条件: ${g.condition})` : ""} [${g.evidenceIds.join(",")}]`);
  }
  if (pack.styleNotes.length > 0) lines.push(`話し方の癖: ${pack.styleNotes.join(" / ")}`);
  return lines;
}

function cardLine(c: StanceCard): string {
  return `- [${c.id}] ${c.polarity === "pos" ? "肯定" : "否定"}: ${c.claim} (根拠 ${c.evidenceIds.join(",")})`;
}

function evidenceLine(e: PackEvidence): string {
  return `- [${e.id}] (${e.gameTitle}・${e.polarity === "pos" ? "好意的" : "否定的"}) ${e.text}`;
}

const RULES = [
  `# 代理の決まり`,
  `- 本人の考えとして言えるのは、上の発言 [E番号] に支えられることだけ。根拠にした発言の id を必ず付ける。`,
  `- 発言に無いことを言うときは「推測」として言い、本人がそう言ったように言わない。`,
  `- 本人の発言に無い好みや属性 (年齢・国・名前など) を作らない。`,
];

function render(pack: PersonaPack, label: string, cards: readonly StanceCard[], evidence: readonly PackEvidence[]): string {
  return [
    ...header(pack, label),
    "",
    `# 本人の主張 (立場カード)`,
    ...(cards.length > 0 ? cards.map(cardLine) : ["- (なし)"]),
    "",
    `# 本人の発言 (原文の抜粋)`,
    ...evidence.map(evidenceLine),
    "",
    ...RULES,
  ].join("\n");
}

/** パックを予算内の束にする。束にできなければ null。 */
export function renderProxyBundle(pack: PersonaPack, options: BundleOptions): ProxyBundle | null {
  if (pack.sufficiency.confidence === "low" || pack.revoked) return null;
  const ordered = orderEvidence(pack.evidence, options.focusTerms ?? []);
  for (let keep = ordered.length; keep >= 1; keep--) {
    const kept = ordered.slice(0, keep);
    const keptIds = new Set(kept.map((e) => e.id));
    const cards = pack.stanceCards
      .map((c) => ({ ...c, evidenceIds: c.evidenceIds.filter((id) => keptIds.has(id)) }))
      .filter((c) => c.evidenceIds.length > 0);
    // 本文に載せる順は id 順 (読みやすさ)。どれを残すかは議題との関係で決めた。
    const shown = [...kept].sort((a, b) => Number(a.id.slice(1)) - Number(b.id.slice(1)));
    const text = render({ ...pack, conditionalGaps: pack.conditionalGaps.filter((g) => g.evidenceIds.every((id) => keptIds.has(id))) }, options.label, cards, shown);
    const tokens = estimateTokens(text);
    if (tokens <= options.budgetTokens) {
      return {
        text,
        evidenceIds: shown.map((e) => e.id),
        evidenceTexts: Object.fromEntries(shown.map((e) => [e.id, e.text])),
        estimatedTokens: tokens,
        droppedEvidence: ordered.length - keep,
      };
    }
  }
  return null;
}
