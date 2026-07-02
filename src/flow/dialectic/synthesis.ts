/**
 * 止揚 [4] — Synthesis 候補の生成 + 折衷ゲート + 敵対的批准 + 修正ループ (respec 06 §2)。
 *
 * - 生成: メインモデル・型別テンプレート (values→条件分割 / means→手段合成 / tradeoff→第 3 案)。
 * - 折衷ゲート (dialectic.md §4): elevates が空/「両方やる」でしかない候補は kind=compromise に
 *   格下げする。折衷は記録するが止揚ストック・早期収束シグナルには数えない。
 * - 批准 (dialectic.md §5-2): 中立審判ではなく「否定するインセンティブを持つ側」= 両陣営の
 *   Position 持ち主にチェックリスト式・**reject デフォルト**で聞く。さらにコードが機械検証:
 *   accept は「自分の根拠の過半数が preservedIds で保存確認できた」場合のみ有効。
 * - 修正ループ: reject の missing を渡して再生成。**コードが最大 MAX_REVISIONS=2 回でカット**。
 *   尽きたら status=rejected → tension=agreed_disagree。
 *
 * コストログは呼び出し側が withCostLog (synthesize / elevation-gate / ratify) でラップして渡す。
 */

import type { LLMClient } from "../../persona-engine/llm/client.js";
import { extractJsonObject } from "../effect-predict.js";
import {
  insertSynthesis,
  updateSynthesis,
  type PositionRecord,
  type RatificationEntry,
  type SynthesisRecord,
  type TensionRecord,
} from "./store.js";

/** 修正ループの上限 (コードが強制カット)。 */
export const MAX_REVISIONS = 2;

// ── 生成 ─────────────────────────────────────────────────────────────────────

/** 型別の止揚テンプレート指示 (dialectic.md §2)。 */
export function synthesisTemplateFor(type: TensionRecord["type"]): string {
  switch (type) {
    case "values":
      return (
        "これは価値対立です。上位価値の発見、または適用条件の分割 (「コア層には X、ライト層には Y」" +
        "のように、どちらの価値も条件付きで生かす分割) で統合してください。"
      );
    case "means":
      return (
        "これは手段対立です (目標は共有)。共通目標を再確認した上で、両者の手段の合成・段階化" +
        " (まず A で始めて条件を満たしたら B へ、など) で統合してください。"
      );
    case "tradeoff":
      return (
        "これはトレードオフです。二者択一に見える 2 変数をパラメータ化し、両方を部分的に満たす" +
        "非線形の第 3 案 (構造を変えることでトレードオフ自体を無効化する案) を出してください。"
      );
    case "facts":
      // 事実対立は本来 [3] で解消済み。防御的に values 相当の指示を返す。
      return "対立する主張を条件分割で統合してください。";
  }
}

export interface GeneratedSynthesis {
  text: string;
  preserves: string[];
  negates: string[];
  elevates: string | null;
  degraded: boolean;
}

export interface GenerateSynthesisArgs {
  issueTitle: string;
  tension: TensionRecord;
  positionA: PositionRecord;
  positionB: PositionRecord;
  /** 前回 reject の欠落根拠 (修正ループ時のフィードバック)。 */
  missingFeedback?: readonly string[];
  /** ペーパー base (system 固定)。 */
  paperSystem: string;
  /** withCostLog 済み生成 LLM (メインモデル, location="synthesize")。 */
  llm: LLMClient;
  model?: string;
  warn?: (msg: string) => void;
}

function renderPositionForSynthesis(label: string, p: PositionRecord): string {
  const grounds = p.grounds.map((g) => `  - [${g.id}] (${g.state}) ${g.text}`).join("\n");
  return `${label} (${p.stance}): ${p.claim}\n${grounds}\n  価値前提: ${p.values.join(" / ") || "(なし)"}`;
}

/** 止揚生成プロンプト (テスト用に export)。 */
export function buildSynthesisPrompt(args: {
  issueTitle: string;
  tension: TensionRecord;
  positionA: PositionRecord;
  positionB: PositionRecord;
  missingFeedback?: readonly string[];
}): string {
  const { issueTitle, tension, positionA, positionB, missingFeedback } = args;
  const feedback =
    missingFeedback && missingFeedback.length > 0
      ? `\n# 前回候補への批准フィードバック (これらの根拠が保存されておらず reject された)\n` +
        missingFeedback.map((m) => `- ${m}`).join("\n") +
        `\n欠けている根拠を保存する形に修正してください。\n`
      : "";
  return (
    `# 論点\n${issueTitle}\n\n` +
    `# 対立する Position (根拠は [id] で指す)\n` +
    `${renderPositionForSynthesis("A", positionA)}\n\n${renderPositionForSynthesis("B", positionB)}\n\n` +
    `# 指示\n${synthesisTemplateFor(tension.type)}\n` +
    feedback +
    `\n止揚 (aufheben) の 3 要件をデータで示してください:\n` +
    `- preserves: 各陣営から何を保存したか (上の根拠 id で指す。両陣営から挙げる)\n` +
    `- negates: 何を放棄させたか (根拠 id)\n` +
    `- elevates: 導入した新しい枠 (上位目標 / 条件分割 / 再定義)。単なる「両方やる」は不可\n\n` +
    `次の JSON 1 個だけを返してください (前後に説明やコードフェンスを付けない):\n` +
    `{"text": "<統合案の口語 1〜3 文>", "preserves": ["<根拠id>"], "negates": ["<根拠id>"], ` +
    `"elevates": "<新しい枠の説明 1 文>"}`
  );
}

function coerceIdArray(v: unknown, valid: ReadonlySet<string>): string[] {
  if (!Array.isArray(v)) return [];
  return v
    .filter((s): s is string => typeof s === "string" && s.trim().length > 0)
    .map((s) => s.trim())
    .filter((id) => valid.has(id));
}

/**
 * Synthesis 候補を 1 つ生成する。LLM 失敗 / パース失敗は degrade
 * (elevates=null = 折衷ゲートで compromise に落ちる)。
 */
export async function generateSynthesis(args: GenerateSynthesisArgs): Promise<GeneratedSynthesis> {
  const { llm, model, warn = () => {} } = args;
  const validIds = new Set([
    ...args.positionA.grounds.map((g) => g.id),
    ...args.positionB.grounds.map((g) => g.id),
  ]);

  const result = await llm.invoke({
    system: args.paperSystem,
    prompt: buildSynthesisPrompt(args),
    ...(model ? { model } : {}),
  });

  if (!result.ok) {
    warn(`Synthesis 生成 LLM エラー: ${result.error} — 空候補で degrade (折衷扱い)`);
    return {
      text: `${args.positionA.claim} と ${args.positionB.claim} の統合案は生成できませんでした`,
      preserves: [],
      negates: [],
      elevates: null,
      degraded: true,
    };
  }

  const obj = extractJsonObject(result.text);
  if (!obj || typeof obj.text !== "string" || !obj.text.trim()) {
    warn(`Synthesis 生成 JSON パース失敗 — 全文を折衷候補として受理 (degrade)`);
    return { text: result.text.trim(), preserves: [], negates: [], elevates: null, degraded: true };
  }

  const elevates =
    typeof obj.elevates === "string" && obj.elevates.trim() ? obj.elevates.trim() : null;
  return {
    text: obj.text.trim(),
    preserves: coerceIdArray(obj.preserves, validIds),
    negates: coerceIdArray(obj.negates, validIds),
    elevates,
    degraded: false,
  };
}

// ── 折衷ゲート ────────────────────────────────────────────────────────────────

/** 「両方やる」系の言い回し (機械前段。LLM 前に明白な折衷を落とす)。 */
const BOTH_PATTERNS = [/両方(と|や|を)?/, /どちらも(やる|行う|採用)/, /both/i];

export interface ElevationGateArgs {
  elevates: string | null;
  /** withCostLog 済み判定 LLM (judgeModel, location="elevation-gate")。 */
  llm: LLMClient;
  model?: string;
  warn?: (msg: string) => void;
}

/**
 * elevation ゲート: elevates が実質空/「両方やる」なら compromise (dialectic.md §4)。
 * 空はコードのみで即決。非空は判定 LLM (単一ラベル)。LLM 失敗は compromise に倒す
 * (reject デフォルトの精神 — 甘い aufheben 認定をしない)。
 */
export async function elevationGate(args: ElevationGateArgs): Promise<"aufheben" | "compromise"> {
  const { elevates, llm, model, warn = () => {} } = args;
  if (!elevates || !elevates.trim()) return "compromise";
  if (BOTH_PATTERNS.some((re) => re.test(elevates)) && elevates.length < 20) {
    // 「両方やる」だけの短文はコードで折衷判定 (LLM 節約 + 決定的)。
    return "compromise";
  }

  const result = await llm.invoke({
    prompt:
      `統合案の「新しい枠」の記述です:\n"${elevates}"\n\n` +
      `これは (a) 上位目標の発見・適用条件の分割・問題の再定義など**新しい枠の導入**か、` +
      `(b) 実質「両方やる」「折半する」でしかない**折衷**か。\n` +
      `aufheben (新しい枠) / compromise (折衷) のどちらか 1 語のみで答えてください。迷ったら compromise。`,
    ...(model ? { model } : {}),
    maxTokens: 50,
  });

  if (!result.ok) {
    warn(`elevation ゲート LLM エラー: ${result.error} — compromise に倒す (degrade)`);
    return "compromise";
  }
  const t = result.text.toLowerCase();
  const ia = t.indexOf("aufheben");
  const ic = t.indexOf("compromise");
  if (ia >= 0 && (ic < 0 || ia < ic)) return "aufheben";
  if (ic >= 0) return "compromise";
  warn(`elevation ゲート ラベル不明: "${result.text.slice(0, 60)}" — compromise に倒す (degrade)`);
  return "compromise";
}

// ── 批准 ─────────────────────────────────────────────────────────────────────

export interface RatifyArgs {
  position: PositionRecord;
  personaName: string;
  synthesisText: string;
  preserves: readonly string[];
  elevates: string | null;
  /** withCostLog 済み判定 LLM (judgeModel, location="ratify")。 */
  llm: LLMClient;
  model?: string;
  warn?: (msg: string) => void;
}

/** 批准プロンプト (spec dialectic.md §5-2 の文言に従う。テスト用に export)。 */
export function buildRatifyPrompt(args: {
  position: PositionRecord;
  personaName: string;
  synthesisText: string;
  preserves: readonly string[];
  elevates: string | null;
}): string {
  const { position, personaName, synthesisText, preserves, elevates } = args;
  const groundIds = position.grounds.map((g) => g.id);
  const groundList = position.grounds.map((g) => `- [${g.id}] ${g.text}`).join("\n");
  return (
    `あなたは議論ペルソナ「${personaName}」。この議論でのあなたの立場は ${position.stance}、` +
    `主張は「${position.claim}」です。\n\n` +
    `# あなたの主張の根拠\n${groundList}\n\n` +
    `# 提示された統合案\n${synthesisText}\n` +
    (elevates ? `新しい枠: ${elevates}\n` : "") +
    `(統合案が保存を主張する根拠: [${preserves.join(", ") || "なし"}])\n\n` +
    `# チェックリスト (敵対的批准 — 甘い受諾をしない)\n` +
    `あなたの主張の根拠は [${groundIds.join(", ")}] だった。` +
    `この統合案で**実際に保存されているもの**を ID で挙げよ。` +
    `保存が確認できた根拠が過半数に満たなければ reject し、欠けているものを指摘せよ。` +
    `**デフォルトは reject** — 保存を自分で確認できたときだけ accept せよ。\n\n` +
    `次の JSON 1 個だけを返してください:\n` +
    `{"verdict": "accept" | "reject", "preservedIds": ["<確認できた根拠id>"], ` +
    `"missing": ["<欠けている根拠id>"], "reason": "<一言>"}`
  );
}

/**
 * 1 陣営の批准。パース失敗 / LLM 失敗は **reject デフォルト**。
 * コードによる機械検証: verdict=accept でも「自分の根拠の過半数が preservedIds で確認できた」
 * 場合のみ accept を有効にする (= 批准された止揚は両陣営の根拠の過半を明示的に保存している)。
 */
export async function ratifySynthesis(args: RatifyArgs): Promise<RatificationEntry> {
  const { position, llm, model, warn = () => {} } = args;
  const ownIds = new Set(position.grounds.map((g) => g.id));
  const rejectDefault = (reason: string): RatificationEntry => ({
    positionId: position.id,
    verdict: "reject",
    preservedIds: [],
    missing: [...ownIds],
    reason,
  });

  const result = await llm.invoke({
    prompt: buildRatifyPrompt(args),
    ...(model ? { model } : {}),
  });
  if (!result.ok) {
    warn(`批准 LLM エラー (${position.stance}): ${result.error} — reject デフォルト`);
    return rejectDefault(`批准 LLM 障害 (${result.error})`);
  }

  const obj = extractJsonObject(result.text);
  if (!obj) {
    warn(`批准 JSON パース失敗 (${position.stance}) — reject デフォルト`);
    return rejectDefault("批准応答を解析できず reject (デフォルト)");
  }

  const preservedIds = Array.isArray(obj.preservedIds)
    ? obj.preservedIds
        .filter((s): s is string => typeof s === "string")
        .map((s) => s.trim())
        .filter((id) => ownIds.has(id))
    : [];
  const missing = Array.isArray(obj.missing)
    ? obj.missing
        .filter((s): s is string => typeof s === "string")
        .map((s) => s.trim())
        .filter((id) => ownIds.has(id))
    : [...ownIds].filter((id) => !preservedIds.includes(id));
  const reason = typeof obj.reason === "string" ? obj.reason : "";

  // 機械検証: 過半数保存が確認できたときのみ accept が有効 (spec 5-2 の保証をコードが持つ)。
  const majority = preservedIds.length > position.grounds.length / 2;
  const verdict: "accept" | "reject" = obj.verdict === "accept" && majority ? "accept" : "reject";
  if (obj.verdict === "accept" && !majority) {
    warn(
      `批准 (${position.stance}): verdict=accept だが保存確認 ${preservedIds.length}/${position.grounds.length} は過半数未満 — コードが reject に矯正`
    );
  }

  return { positionId: position.id, verdict, preservedIds, missing, reason };
}

// ── 修正ループ (コードが最大 2 回でカット) ───────────────────────────────────

export interface SynthesisLoopArgs {
  issueTitle: string;
  tension: TensionRecord;
  positionA: PositionRecord;
  positionB: PositionRecord;
  personaNames: ReadonlyMap<string, string>;
  paperSystem: string;
  /** withCostLog 済み: 生成 (メインモデル)。 */
  genLlm: LLMClient;
  /** withCostLog 済み: 折衷ゲート (judgeModel)。 */
  gateLlm: LLMClient;
  /** withCostLog 済み: 批准 (judgeModel)。 */
  ratifyLlm: LLMClient;
  mainModel?: string;
  judgeModel?: string;
  warn?: (msg: string) => void;
}

export interface SynthesisOutcome {
  record: SynthesisRecord;
  /** tension の帰結: synthesized (止揚批准) / compromised (折衷批准) / agreed_disagree (批准不成立)。 */
  tensionStatus: "synthesized" | "compromised" | "agreed_disagree";
}

/**
 * 止揚の生成 → 折衷ゲート → 敵対的批准 → 修正ループを回す。
 * 修正は最大 MAX_REVISIONS 回 (コードがカット)。全 reject なら agreed_disagree。
 * flow_synthesis には 1 行を作り、修正のたびに同一行を上書きする (revision カウント)。
 */
export async function runSynthesisLoop(args: SynthesisLoopArgs): Promise<SynthesisOutcome> {
  const { tension, positionA, positionB, warn = () => {} } = args;
  const nameOf = (p: PositionRecord): string => args.personaNames.get(p.personaId) ?? p.personaId;

  let recordId: string | null = null;
  let missingFeedback: string[] = [];
  let last: SynthesisRecord | null = null;

  for (let revision = 0; revision <= MAX_REVISIONS; revision++) {
    const candidate = await generateSynthesis({
      issueTitle: args.issueTitle,
      tension,
      positionA,
      positionB,
      missingFeedback: missingFeedback.length > 0 ? missingFeedback : undefined,
      paperSystem: args.paperSystem,
      llm: args.genLlm,
      model: args.mainModel,
      warn,
    });

    const kind = await elevationGate({
      elevates: candidate.elevates,
      llm: args.gateLlm,
      model: args.judgeModel,
      warn,
    });

    const ratifications: RatificationEntry[] = [];
    for (const position of [positionA, positionB]) {
      ratifications.push(
        await ratifySynthesis({
          position,
          personaName: nameOf(position),
          synthesisText: candidate.text,
          preserves: candidate.preserves,
          elevates: candidate.elevates,
          llm: args.ratifyLlm,
          model: args.judgeModel,
          warn,
        })
      );
    }

    const allAccepted = ratifications.every((r) => r.verdict === "accept");
    const status: SynthesisRecord["status"] = allAccepted
      ? "ratified"
      : revision < MAX_REVISIONS
        ? "revising"
        : "rejected";

    const snapshot: Omit<SynthesisRecord, "id"> = {
      tensionId: tension.id,
      preserves: candidate.preserves,
      negates: candidate.negates,
      elevates: candidate.elevates,
      kind,
      text: candidate.text,
      ratification: ratifications,
      status,
      revision,
    };

    if (recordId === null) {
      last = insertSynthesis(snapshot);
      recordId = last.id;
    } else {
      updateSynthesis(recordId, snapshot);
      last = { ...snapshot, id: recordId };
    }

    if (allAccepted) {
      return { record: last, tensionStatus: kind === "aufheben" ? "synthesized" : "compromised" };
    }

    missingFeedback = ratifications.flatMap((r) => (r.verdict === "reject" ? r.missing : []));
    warn(
      `Synthesis 批准 reject (revision ${revision}/${MAX_REVISIONS}): 欠落根拠 [${missingFeedback.join(", ")}]` +
        (revision < MAX_REVISIONS ? " — 修正ループへ" : " — 打ち切り (agreed_disagree)")
    );
  }

  // MAX_REVISIONS を使い切って批准不成立 → 合意不能として記録 (握り潰さない)。
  return { record: last!, tensionStatus: "agreed_disagree" };
}
