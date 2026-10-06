/**
 * すり合わせ [4] — 止揚に代わる Tension の決着 (dialectic.md §4.5)。
 *
 * 議論のゴールは統合案ではなく、次の 3 点のすり合わせ:
 *   - どんな体験をつくりたいか (experience)
 *   - どんな施策を試したいか (measure)
 *   - 他のゲームではどうしているか / どうなっているか (references)
 * に加えて、両陣営の根拠のうち合意できたもの (agreed) と、まだ決まらない点 (open = 何が分かれば決まるか)
 * を整理する。何の数字を改善したいかは算出しない (施策の主張は人間が定義する)。
 *
 * 生成は進行役モデル 1 call。コードが検証する:
 *   - agreed / open の根拠 id は両 Position の未譲歩の根拠に限る (不明 id は捨てる)
 *   - 両方に挙がった id は open を優先する (合意を水増ししない)
 *   - どちらにも挙がらなかった未譲歩の根拠は open に補う (黙って落とさない)
 * LLM 障害・パース失敗は全根拠を open にして degrade (議論を止めない、warn 明示)。
 */

import type { LLMClient } from "../../persona-engine/llm/client.js";
import { extractJsonObject } from "../effect-predict.js";
import { coerceAttackPoint, INFERENCE_KIND_LABEL } from "./premise-rules.js";
import type { AlignmentRecord, OpenPoint } from "./alignment-store.js";
import type { PositionRecord, TensionRecord } from "./store.js";

export type GeneratedAlignment = Omit<AlignmentRecord, "id" | "tensionId"> & { degraded: boolean };

/** 未譲歩の根拠 (すり合わせの対象)。 */
function liveGroundIds(positions: readonly PositionRecord[]): string[] {
  return positions.flatMap((p) => p.grounds.filter((g) => g.state !== "conceded").map((g) => g.id));
}

function renderPosition(label: string, p: PositionRecord): string {
  const grounds = p.grounds
    .filter((g) => g.state !== "conceded")
    .map((g) => {
      const kind = g.kind ? ` ${INFERENCE_KIND_LABEL[g.kind]}` : "";
      const warrant = g.warrant ? ` / 論拠: ${g.warrant}` : "";
      return `  - [${g.id}] (${g.state}${kind}) データ: ${g.text}${warrant}`;
    })
    .join("\n");
  return `${label} (${p.stance}): ${p.claim}\n  限定: ${p.qualifier ?? "(明示なし)"}\n${grounds}`;
}

/** すり合わせ生成プロンプト (テスト用に export)。 */
export function buildAlignmentPrompt(args: {
  issueTitle: string;
  tension: TensionRecord;
  positionA: PositionRecord;
  positionB: PositionRecord;
}): string {
  return (
    `# 論点\n${args.issueTitle}\n\n` +
    `# 対立している 2 つの立場 (譲歩済みの根拠は除外済み)\n` +
    `${renderPosition("A", args.positionA)}\n\n${renderPosition("B", args.positionB)}\n\n` +
    `# 指示 (すり合わせ)\n` +
    `統合案を作るのではなく、両者の主張と根拠をすり合わせて次を整理してください。\n` +
    `- experience: この論点でつくりたい体験\n` +
    `- measure: 試したい施策 (アイデア)\n` +
    `- references: 他のゲームではどうしているか、どうなっているか (根拠に出た事例から。無ければ空文字)\n` +
    `- agreed: 両者が受け入れられる根拠の id\n` +
    `- open: まだ決まらない点。point は data / warrant / qualifier のどれが未決か、need は何が分かれば決まるか\n` +
    `何の数字を改善すべきかは決めない。根拠に無い事実を足さない。\n` +
    `次の JSON 1 個だけを返してください (前後に説明やコードフェンスを付けない):\n` +
    `{"experience": "...", "measure": "...", "references": "...", "agreed": ["<根拠 id>"], ` +
    `"open": [{"groundId": "<根拠 id。限定そのものなら null>", "point": "<data|warrant|qualifier>", "need": "..."}], ` +
    `"text": "<Discord に流すまとめ。口語 2〜4 文>"}`
  );
}

function str(v: unknown): string {
  return typeof v === "string" ? v.trim() : "";
}

/**
 * LLM 応答を検証してすり合わせにする (純関数・テスト用に export)。
 * 応答が JSON でなければ null。
 */
export function parseAlignment(
  raw: string,
  positions: readonly PositionRecord[],
): Omit<GeneratedAlignment, "degraded"> | null {
  const obj = extractJsonObject(raw);
  if (!obj) return null;
  const live = new Set(liveGroundIds(positions));

  const open: OpenPoint[] = [];
  const openIds = new Set<string>();
  for (const item of Array.isArray(obj.open) ? obj.open : []) {
    if (!item || typeof item !== "object") continue;
    const o = item as Record<string, unknown>;
    const groundId = str(o.groundId);
    const point = coerceAttackPoint(o.point) ?? "warrant";
    const need = str(o.need);
    if (!need) continue;
    if (groundId && !live.has(groundId)) continue;
    if (groundId) openIds.add(groundId);
    open.push({ groundId: groundId || null, point, need });
  }

  const agreed: string[] = [];
  for (const id of Array.isArray(obj.agreed) ? obj.agreed : []) {
    const gid = str(id);
    if (gid && live.has(gid) && !openIds.has(gid) && !agreed.includes(gid)) agreed.push(gid);
  }

  for (const gid of live) {
    if (!openIds.has(gid) && !agreed.includes(gid)) {
      open.push({ groundId: gid, point: "warrant", need: "この根拠はすり合わせで扱われなかった。受け入れられるかを確認する" });
    }
  }

  return {
    experience: str(obj.experience),
    measure: str(obj.measure),
    references: str(obj.references),
    agreed,
    open,
    text: str(obj.text),
  };
}

/** 全根拠を未決にした degrade 結果。 */
function degradedAlignment(positions: readonly PositionRecord[], note: string): GeneratedAlignment {
  return {
    experience: "",
    measure: "",
    references: "",
    agreed: [],
    open: liveGroundIds(positions).map((gid) => ({ groundId: gid, point: "warrant" as const, need: note })),
    text: "この論点はすり合わせを整理できませんでした。根拠はすべて未決として残します。",
    degraded: true,
  };
}

export interface GenerateAlignmentArgs {
  issueTitle: string;
  tension: TensionRecord;
  positionA: PositionRecord;
  positionB: PositionRecord;
  /** ペーパー base (system 固定)。 */
  paperSystem: string;
  /** withCostLog 済み LLM (location="align")。 */
  llm: LLMClient;
  model?: string;
  warn?: (msg: string) => void;
}

/** すり合わせを 1 件生成する。 */
export async function generateAlignment(args: GenerateAlignmentArgs): Promise<GeneratedAlignment> {
  const { llm, model, warn = () => {} } = args;
  const positions = [args.positionA, args.positionB];
  const result = await llm.invoke({
    system: args.paperSystem,
    prompt: buildAlignmentPrompt(args),
    ...(model ? { model } : {}),
  });
  if (!result.ok) {
    warn(`すり合わせ生成 LLM エラー: ${result.error} — 全根拠を未決として degrade`);
    return degradedAlignment(positions, "すり合わせの生成に失敗した。人間が確認する");
  }
  const parsed = parseAlignment(result.text, positions);
  if (!parsed) {
    warn(`すり合わせ JSON パース失敗 — 全根拠を未決として degrade: ${result.text.slice(0, 60)}`);
    return degradedAlignment(positions, "すり合わせの応答を読めなかった。人間が確認する");
  }
  return { ...parsed, text: parsed.text || renderAlignmentText(parsed), degraded: false };
}

/** 露出文が空のときの機械組み立て。 */
export function renderAlignmentText(a: Pick<AlignmentRecord, "experience" | "measure" | "references" | "open">): string {
  const parts = [
    a.experience && `つくりたい体験は「${a.experience}」`,
    a.measure && `試したい施策は「${a.measure}」`,
    a.references && `他のゲームでは「${a.references}」`,
  ].filter(Boolean);
  const head = parts.length > 0 ? `${parts.join("、")}という整理です。` : "整理できた点はまだありません。";
  return a.open.length > 0 ? `${head}まだ決まらない点が ${a.open.length} 件あります。` : `${head}根拠はすべて合意できました。`;
}

/** すり合わせ結果 → Tension の状態。 */
export function alignmentStatus(a: Pick<AlignmentRecord, "open">): "aligned" | "partially_aligned" {
  return a.open.length === 0 ? "aligned" : "partially_aligned";
}
