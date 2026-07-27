/**
 * 仕様・施策議論の入力を HTTP/UI から独立して正規化する。
 *
 * freeform は既存フローとの後方互換、specification / policy-list は
 * 「議論仕様書または施策リストを用意する」明示フローで必須入力を fail-fast する。
 */

export type DiscussionInputKind = "freeform" | "specification" | "policy-list";

export interface PolicyDiscussionInput {
  kind: DiscussionInputKind;
  policyItems: string[];
}

function inputKind(value: unknown): DiscussionInputKind {
  if (value === undefined || value === null || value === "") return "freeform";
  if (value === "freeform" || value === "specification" || value === "policy-list") return value;
  throw new TypeError(`unsupported discussionInputKind: ${String(value)}`);
}

export function parsePolicyItems(value: unknown): string[] {
  const lines = Array.isArray(value)
    ? value
    : typeof value === "string"
      ? value.split(/\r?\n/)
      : [];
  return [...new Set(
    lines
      .filter((item): item is string => typeof item === "string")
      .map((item) => item.replace(/^(?:\d+[.)]\s+|[-*]\s+)/, "").trim())
      .filter(Boolean)
  )];
}

function hasSpecification(body: Record<string, unknown>): boolean {
  return ["mechanicsContext", "specText", "specUrl", "githubRepoUrl"]
    .some((key) => typeof body[key] === "string" && body[key].trim().length > 0);
}

export function parsePolicyDiscussionInput(body: Record<string, unknown>): PolicyDiscussionInput {
  const kind = inputKind(body.discussionInputKind);
  const policyItems = parsePolicyItems(body.policyItems);
  if (kind === "specification" && !hasSpecification(body)) {
    throw new TypeError("仕様書の本文、ファイル、URL、または GitHub ファイルを指定してください");
  }
  if (kind === "policy-list" && policyItems.length === 0) {
    throw new TypeError("施策を1件以上入力してください");
  }
  return { kind, policyItems };
}

/** 施策リストを既存の議論内容へ追記し、ペーパー/議論プロンプトの共通経路へ載せる。 */
export function applyPolicyItems(
  discussionContent: unknown,
  policyItems: readonly string[]
): string | undefined {
  const current = typeof discussionContent === "string" ? discussionContent.trim() : "";
  if (policyItems.length === 0) return current || undefined;
  const list = policyItems.map((item, index) => `${index + 1}. ${item}`).join("\n");
  return [current, `検討する施策リスト:\n${list}`].filter(Boolean).join("\n\n");
}
