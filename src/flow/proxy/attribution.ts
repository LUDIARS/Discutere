/**
 * 代理の主張の帰属検査 (統合レビュー §1「帰属の分類」「検査」、S3)。
 *
 * 代理が出す論拠 1 件ごとに、本人の発言との関係を 4 区分で申告させ、コードで検査する:
 *   - sourceQuote        : 本人の発言の引用。引いた [E番号] が束にあり、引用部が原文に含まれること
 *   - groundedParaphrase : 本人の発言に基づく言い換え。引いた [E番号] が束にあること
 *   - simulationInference: 本人の傾向からの推測。本人の発言としては扱わない (露出に「推測」と付ける)
 *   - unknown            : 材料が無くて分からない
 * 検査に通らない「本人の発言」申告は採らない (呼び出し側が問いに格下げ / 推測に落とす)。
 * 決定的 (LLM なし)。検査ができない状態を ok にしない。
 */

export type ClaimKind = "sourceQuote" | "groundedParaphrase" | "simulationInference" | "unknown";

export const CLAIM_KINDS: readonly ClaimKind[] = ["sourceQuote", "groundedParaphrase", "simulationInference", "unknown"];

/** 本人の発言として扱う区分。 */
export function isGroundedKind(kind: ClaimKind): boolean {
  return kind === "sourceQuote" || kind === "groundedParaphrase";
}

export function coerceClaimKind(v: unknown): ClaimKind | null {
  return typeof v === "string" && (CLAIM_KINDS as readonly string[]).includes(v) ? (v as ClaimKind) : null;
}

export function coerceEvidenceIds(v: unknown): string[] {
  if (!Array.isArray(v)) return [];
  const out: string[] = [];
  for (const item of v) {
    if (typeof item !== "string") continue;
    const id = item.trim().replace(/^\[|\]$/g, "");
    if (/^E\d{1,3}$/.test(id) && !out.includes(id)) out.push(id);
  }
  return out;
}

/** 代理が引いてよい根拠 (束に載った発言)。 */
export interface AttributionScope {
  evidenceIds: readonly string[];
  evidenceTexts: Readonly<Record<string, string>>;
}

export type AttributionFoul = "unsupported_attribution" | "quote_mismatch";

export type AttributionResult =
  | { ok: true; claimKind: ClaimKind; evidenceIds: string[] }
  | { ok: false; foul: AttributionFoul; claimKind: ClaimKind; evidenceIds: string[] };

const QUOTE_SEGMENT = /[「『"“]([^」』"”]{4,})[」』"”]/g;

function normalize(s: string): string {
  return s.replace(/\s+/g, "");
}

/** 引用部 (「」や "" で囲んだ部分、無ければ data 全体) が引いた発言のどれかに含まれるか。 */
export function quoteAppearsInEvidence(data: string, cited: readonly string[]): boolean {
  const segments = [...data.matchAll(QUOTE_SEGMENT)].map((m) => normalize(m[1]));
  const targets = segments.length > 0 ? segments : [normalize(data)];
  const sources = cited.map(normalize);
  return targets.some((t) => t.length >= 4 && sources.some((s) => s.includes(t)));
}

/**
 * 1 件の主張を検査する。claimKind 未申告は推測として扱う (本人の発言にはしない)。
 */
export function checkAttribution(args: {
  claimKind: ClaimKind | null;
  evidenceIds: readonly string[];
  data: string;
  scope: AttributionScope;
}): AttributionResult {
  const claimKind = args.claimKind ?? "simulationInference";
  const allowed = new Set(args.scope.evidenceIds);
  const cited = args.evidenceIds.filter((id) => allowed.has(id));
  if (!isGroundedKind(claimKind)) return { ok: true, claimKind, evidenceIds: cited };
  if (args.evidenceIds.length === 0 || cited.length !== args.evidenceIds.length) {
    return { ok: false, foul: "unsupported_attribution", claimKind, evidenceIds: cited };
  }
  if (claimKind === "sourceQuote") {
    const texts = cited.map((id) => args.scope.evidenceTexts[id] ?? "");
    if (!quoteAppearsInEvidence(args.data, texts)) {
      return { ok: false, foul: "quote_mismatch", claimKind, evidenceIds: cited };
    }
  }
  return { ok: true, claimKind, evidenceIds: cited };
}

/** 露出文に付ける帰属の印 (本人の発言か推測かを読み手が区別できるように)。 */
export function attributionSuffix(claimKind: ClaimKind, evidenceIds: readonly string[]): string {
  if (isGroundedKind(claimKind)) return evidenceIds.length > 0 ? ` [${evidenceIds.join(",")}]` : "";
  return claimKind === "unknown" ? "（材料不足）" : "（推測）";
}
