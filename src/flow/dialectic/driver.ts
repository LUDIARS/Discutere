/**
 * dialectic 位相ドライバ (respec 05/06, dialectic.md §3) — 論証状態機械の進行台本。
 *
 *   [0] 論点分解 (paperOverride.issues があれば再分解しない)
 *   [1] 定立 (issue × pro/con debater の Position 生成)
 *   [2] 反定立 (スケジューラ指名 + acts 付き発話 → 論証グラフ更新)
 *   [3] 矛盾特定 (型分類。facts → RAG 解消 / 失敗は unresolved_fact)
 *   [4] 止揚 (型別テンプレ生成 → 折衷ゲート → 敵対的批准 → 修正ループ ≤2)
 *   [5] 計測型収束 (2/3 シグナル + aufhebungTarget 上限キャップ)
 *
 * 状態遷移・スケジューリング・収束判定はすべて決定的なコード。LLM は生成と敵対的批准のみ。
 * FlowDirectorResult 互換の結果を返し (dispatch の戻り型は不変)、永続は既存関数
 * (setupFlowPaper→persistPaper / persistUtterance / generateConclusion 系) を流用するため、
 * 学習ビュー・議論一覧・Discord/Web の露出面は rounds エンジンと同じ経路で動く。
 * rounds エンジン (runFlow) は完全無改変で併存する (flow.engine 既定 "rounds")。
 */

import { randomUUID } from "node:crypto";
import { getConfig } from "../../config.js";
import type { LLMClient } from "../../persona-engine/llm/client.js";
import type { FlowTag } from "../tags.js";
import {
  persistUtterance,
  type FlowDirectorResult,
  type FlowRunOptions,
  type FlowUtteranceRecord,
} from "../director.js";
import { personaStance, type FlowPersona, type Rng } from "../personas.js";
import { buildFlowCast } from "../persona-cast.js";
import { setupSessionPersonas } from "../persona-setup.js";
import { setupFlowPaper } from "../flow-setup.js";
import { createVoiceCache } from "../voice-cache.js";
import { updatePaperBody, type RoundSummary } from "../discussion-paper.js";
import { renderProgressMarkdown } from "../paper-markdown.js";
import { withCostLog } from "../cost-logger.js";
import { runRoundVote, type VoteResult } from "../vote.js";
import { ArgumentGraph } from "../argument-graph.js";
import { decomposeIssues, ISSUES_MAX } from "./issues.js";
import {
  insertIssue,
  insertPosition,
  insertTension,
  updateIssueStatus,
  updatePositionGrounds,
  updateTensionStatus,
  type IssueRecord,
  type PositionRecord,
  type SynthesisRecord,
  type TensionRecord,
} from "./store.js";
import { generatePosition } from "./position.js";
import { buildDialecticTurnPrompt, parseTurnResponse, type UtteranceDigest } from "./turn-prompt.js";
import {
  renderFacilitatorLine,
  renderNomination,
  scheduleNextTurn,
  type PositionAnchor,
} from "./scheduler.js";
import { classifyTension, resolveFactTension } from "./tension.js";
import { runSynthesisLoop } from "./synthesis.js";
import { assessConvergence } from "./convergence.js";
import { concludeDialectic, settlementToRoundSummary, type IssueSettlement } from "./conclusion.js";

function defaultRng(): number {
  return Math.random();
}

/** pro/con の debater を確保する (不足時は非 facilitator を昇格して degrade。warn 明示)。 */
export function ensureDebaters(
  cast: readonly FlowPersona[],
  warn: (msg: string) => void
): { pro: FlowPersona; con: FlowPersona; cast: FlowPersona[] } {
  const working = [...cast];
  const findDebater = (stance: "pro" | "con") =>
    working.find((p) => p.role === "debater" && p.stance === stance);

  let pro = findDebater("pro");
  let con = findDebater("con");
  if (pro && con) return { pro, con, cast: working };

  for (const stance of ["pro", "con"] as const) {
    if (stance === "pro" ? pro : con) continue;
    const idx = working.findIndex(
      (p) => p.role !== "facilitator" && p.id !== pro?.id && p.id !== con?.id && !(p.role === "debater" && (p.stance === "pro" || p.stance === "con"))
    );
    if (idx < 0) {
      throw new Error(
        "dialectic エンジンには pro/con の debater が 2 人必要です (flow.personaCount を 4 以上にしてください)"
      );
    }
    const promoted: FlowPersona = { ...working[idx], role: "debater", stance };
    working[idx] = promoted;
    warn(`debater 不足: 「${promoted.name}」を ${stance} 側 debater に昇格 (degrade)`);
    if (stance === "pro") pro = promoted;
    else con = promoted;
  }
  return { pro: pro!, con: con!, cast: working };
}

/** rebut 発話 → 攻撃対象根拠 の対応 (ground 状態の write-through 同期に使う)。 */
interface RebutGroundLink {
  position: PositionRecord;
  groundId: string;
}

/**
 * dialectic エンジンで議論フローを実行する。runFlow (rounds) と同じ
 * FlowDirectorResult を返す (dispatch / 露出面の互換)。
 */
export async function runDialecticFlow(
  theme: string,
  tags: readonly FlowTag[],
  options: FlowRunOptions
): Promise<FlowDirectorResult> {
  const cfg = getConfig();
  const {
    llm,
    listExternalVoices = () => [],
    youtubeSearch,
    onUtterance,
    log = (m) => console.log(`[flow/dialectic] ${m}`),
    warn = (m) => console.warn(`[flow/dialectic/warn] ${m}`),
    rng = defaultRng,
    gamesDir,
    flow = "discussion",
  } = options;

  const sessionId = options.sessionId ?? randomUUID();
  const isLocal = cfg.llm.backend === "local";
  const judgeModel = cfg.flow.dialectic.judgeModel || undefined;
  const voiceCache = createVoiceCache(listExternalVoices);

  // ── ペーパー初期化 (既存 setupFlowPaper 流用: investigate 省略/増補/persistPaper 込み) ──
  const { paper, paperId } = await setupFlowPaper({
    theme,
    tags,
    flow,
    sessionId,
    llm,
    voiceCache,
    youtubeSearch,
    gamesDir,
    paperOverride: options.paperOverride,
    log,
    warn,
  });
  // setupFlowPaper は全経路で bodyMd を持たせる。system 固定 (プロンプトキャッシュ戦略の維持)。
  const paperSystem = paper.bodyMd ?? "";

  // ── キャスト (stance セッション固定 + 価値軸/核主張 + flow_session_persona 永続) ──
  const defaultModel = cfg.llm.model ?? "claude-haiku-4-5-20251001";
  const seedCast = buildFlowCast({
    count: cfg.flow.personaCount,
    defaultModel,
    isLocal,
    rng: rng as Rng,
    personaIds: options.personaIds,
    warn,
  });
  const enriched = await setupSessionPersonas({
    theme,
    sessionId,
    personas: seedCast,
    llm,
    flow,
    model: cfg.flow.personaSetupModel || undefined,
    warn,
  });
  const { pro, con, cast } = ensureDebaters(enriched, warn);
  const facilitator = cast.find((p) => p.role === "facilitator") ?? cast[0];
  const speakers = cast.filter((p) => p.role !== "facilitator");
  const personaNames = new Map(cast.map((p) => [p.id, p.name]));
  log(
    `ペルソナ ${cast.length} 人: ${cast.map((p) => `${p.name}(${p.role}/${p.stance})`).join(", ")} / engine=dialectic`
  );

  // ── セッション状態 (決定的コードが持つ) ──────────────────────────────────
  const graph = new ArgumentGraph();
  const allUtterances: FlowUtteranceRecord[] = [];
  const utteranceIndex = new Map<string, UtteranceDigest>();
  const speakCounts = new Map<string, number>();
  const voteResults: VoteResult[] = [];
  const settlements: IssueSettlement[] = [];
  const ratifiedAufheben: string[] = [];
  const rebutGroundLinks = new Map<string, RebutGroundLink>();
  let globalTurn = 0;

  /** 発話の永続 + 通知 + インデックス更新 (persistUtterance → onUtterance の順序維持)。 */
  const commit = async (record: FlowUtteranceRecord): Promise<void> => {
    persistUtterance(record);
    allUtterances.push(record);
    utteranceIndex.set(record.id, { id: record.id, personaName: record.personaName, text: record.text });
    if (record.role !== "facilitator" && !record.isError) {
      speakCounts.set(record.personaId, (speakCounts.get(record.personaId) ?? 0) + 1);
    }
    if (onUtterance) await onUtterance(record);
  };

  const makeRecord = (args: {
    persona: FlowPersona;
    round: number;
    turn: number;
    text: string;
    act?: string;
    targetId?: string;
    isError?: boolean;
  }): FlowUtteranceRecord => ({
    id: randomUUID(),
    sessionId,
    paperId,
    round: args.round,
    turn: args.turn,
    personaId: args.persona.id,
    personaName: args.persona.name,
    role: args.persona.role,
    stance: personaStance(args.persona),
    text: args.text,
    isError: args.isError ?? false,
    possessionName: args.persona.possession?.label,
    ...(args.act ? { act: args.act } : {}),
    ...(args.targetId ? { targetId: args.targetId } : {}),
  });

  /** graph の rebut 辺状態を Position の根拠状態へ write-through 同期する。 */
  const syncGroundStates = (): void => {
    const touched = new Set<PositionRecord>();
    for (const [rebutId, link] of rebutGroundLinks) {
      const edge = graph.rebuttalById(rebutId);
      if (!edge) continue;
      const ground = link.position.grounds.find((g) => g.id === link.groundId);
      if (!ground) continue;
      const next =
        edge.state === "defended" ? "defended" : edge.state === "conceded" ? "conceded" : "challenged";
      if (ground.state !== next) {
        ground.state = next;
        touched.add(link.position);
      }
    }
    for (const p of touched) updatePositionGrounds(p.id, p.grounds);
  };

  const facilitatorLlm = withCostLog(llm, { flow, sessionId, role: "facilitator", persona: facilitator.name, location: "facilitator" });

  // ── [0] 論点分解 (paperOverride.issues があれば再分解しない — PR-D 共用) ────
  const override = options.paperOverride;
  let issueTitles: string[];
  let issueSource: IssueRecord["source"];
  const approved = (override?.issues ?? []).map((s) => s.trim()).filter(Boolean);
  if (approved.length > 0) {
    issueTitles = approved.slice(0, ISSUES_MAX);
    issueSource = "paper-gate";
    log(`[0] 承認済み論点 ${issueTitles.length} 件を採用 (再分解しない)`);
  } else {
    issueSource = "llm";
    try {
      const decomposeLlm = withCostLog(llm, { flow, sessionId, location: "issue-decompose" });
      issueTitles = await decomposeIssues({ theme, paperMd: paperSystem, llm: decomposeLlm, warn });
    } catch (e) {
      warn(`[0] 論点分解失敗: ${(e as Error).message} — テーマ 1 論点で degrade`);
      issueTitles = [];
    }
    if (issueTitles.length === 0) {
      warn("[0] 論点 0 件 — テーマそのものを単一論点として続行 (degrade)");
      issueTitles = [theme];
    }
    log(`[0] 論点分解: ${issueTitles.length} 件`);
  }

  const issues: IssueRecord[] = issueTitles.map((title, i) =>
    insertIssue({ sessionId, title, ordinal: i + 1, source: issueSource, status: "open" })
  );

  // ── issue ループ ([1]〜[5] は issue ごとに独立に回る) ─────────────────────
  let convergedEarly = false;
  for (const issue of issues) {
    if (convergedEarly) break; // 収束後の issue は open のまま結論に「未討議」で残る
    const round = issue.ordinal;
    log(`論点 ${round}/${issues.length}: ${issue.title}`);

    // ファシリテーター開幕 (露出文のレンダリングのみ LLM)。
    const openingText = await renderFacilitatorLine({
      content: `論点 ${round}「${issue.title}」について議論します。まず賛成側の${pro.name}さん、続いて反対側の${con.name}さんに定立をお願いします。`,
      llm: facilitatorLlm,
      model: judgeModel,
      warn,
    });
    await commit(makeRecord({ persona: facilitator, round, turn: 0, text: openingText }));

    // ── [1] 定立 ──────────────────────────────────────────────────────────
    const positions: PositionRecord[] = [];
    const anchors: PositionAnchor[] = [];
    const positionByUtterance = new Map<string, PositionRecord>();
    let turn = 0;
    for (const persona of [pro, con]) {
      turn += 1;
      const stance = persona.stance === "con" ? "con" : "pro";
      const positionLlm = withCostLog(llm, {
        flow,
        sessionId,
        round,
        turn,
        role: persona.role,
        persona: persona.name,
        location: "position",
      });
      const gp = await generatePosition({
        theme,
        issue,
        persona,
        stance,
        paperSystem,
        llm: positionLlm,
        model: persona.model,
        warn,
      });
      const position = insertPosition({
        issueId: issue.id,
        personaId: persona.id,
        stance,
        claim: gp.claim,
        grounds: gp.grounds,
        values: gp.values,
      });
      positions.push(position);
      const record = makeRecord({ persona, round, turn, text: gp.text, act: "claim" });
      await commit(record);
      graph.apply({ id: record.id, personaId: persona.id, act: "claim", targetId: null, turn: ++globalTurn });
      anchors.push({ personaId: persona.id, stance, utteranceId: record.id });
      positionByUtterance.set(record.id, position);
    }

    // ── [2] 反定立 (スケジューラ指名 + acts 付き発話 → グラフ更新) ─────────
    const rebutTurns = Math.max(0, cfg.flow.dialectic.rebutTurns);
    for (let t = 0; t < rebutTurns; t++) {
      const decision = scheduleNextTurn({ graph, speakers, positionAnchors: anchors, speakCounts });
      if (!decision) break;
      const persona = cast.find((p) => p.id === decision.personaId);
      if (!persona) break;
      turn += 1;

      // 応答権指名はファシリテーターが露出文で指名する (レンダリングのみ LLM)。
      if (decision.kind === "respond") {
        const nomination = await renderNomination({
          decision,
          personaName: persona.name,
          target: decision.targetUtteranceId ? utteranceIndex.get(decision.targetUtteranceId) : undefined,
          llm: facilitatorLlm,
          model: judgeModel,
          warn,
        });
        await commit(makeRecord({ persona: facilitator, round, turn, text: nomination }));
      }

      const turnLlm = withCostLog(llm, {
        flow,
        sessionId,
        round,
        turn,
        role: persona.role,
        persona: persona.name,
        location: "utterance",
      });
      const prompt = buildDialecticTurnPrompt({
        persona,
        stance: personaStance(persona),
        issue,
        positions,
        unresolvedRebuttals: graph.unresolvedRebuttals(),
        utterances: utteranceIndex,
        personaNames,
        allowedActs: decision.allowedActs,
        targetUtteranceId: decision.targetUtteranceId ?? undefined,
      });
      const result = await turnLlm.invoke({ system: paperSystem, prompt, model: persona.model });

      if (!result.ok) {
        warn(`論点 ${round} ターン ${turn} (${persona.name}) LLM エラー: ${result.error}`);
        await commit(
          makeRecord({ persona, round, turn, text: `[エラー: ${result.error}]`, isError: true })
        );
        continue;
      }

      const parsed = parseTurnResponse(result.text, {
        allowedActs: decision.allowedActs,
        validTargets: new Set(utteranceIndex.keys()),
        defaultTargetId: decision.targetUtteranceId,
        warn,
      });
      if (!parsed.text) {
        log(`論点 ${round} ターン ${turn} (${persona.name}): 空応答 (スキップ)`);
        continue;
      }
      const record = makeRecord({
        persona,
        round,
        turn,
        text: parsed.text,
        act: parsed.act,
        targetId: parsed.targetId ?? undefined,
      });
      await commit(record);
      graph.apply({
        id: record.id,
        personaId: persona.id,
        act: parsed.act,
        targetId: parsed.targetId,
        turn: ++globalTurn,
      });

      // rebut が Position を狙った場合: 攻撃対象の根拠を紐付け (challenged へ)。
      if (parsed.act === "rebut" && parsed.targetId) {
        const position = positionByUtterance.get(parsed.targetId);
        if (position) {
          const ground =
            (parsed.groundId && position.grounds.find((g) => g.id === parsed.groundId && g.state !== "conceded")) ||
            position.grounds.find((g) => g.state === "unchallenged") ||
            position.grounds.find((g) => g.state !== "conceded");
          if (ground) rebutGroundLinks.set(record.id, { position, groundId: ground.id });
        }
      }
      syncGroundStates();
    }

    // ── [3] 矛盾の特定 (型分類 + 事実解消) ─────────────────────────────────
    const [positionA, positionB] = positions;
    const tensions: TensionRecord[] = [];
    const syntheses: SynthesisRecord[] = [];
    const aAlive = positionA.grounds.some((g) => g.state !== "conceded");
    const bAlive = positionB.grounds.some((g) => g.state !== "conceded");

    if (aAlive && bAlive) {
      const classifyLlm = withCostLog(llm, { flow, sessionId, round, location: "tension-classify" });
      const type = await classifyTension({ issue, positionA, positionB, llm: classifyLlm, model: judgeModel, warn });
      const tension = insertTension({
        issueId: issue.id,
        positionA: positionA.id,
        positionB: positionB.id,
        type,
        status: "open",
        resolutionNote: null,
      });
      tensions.push(tension);
      log(`論点 ${round} の対立型: ${type}`);

      if (type === "facts") {
        // 事実対立は弁証法のスコープ外: RAG 照会 → 判定。解消不能は結論に明記。
        const factLlm = withCostLog(llm, { flow, sessionId, round, location: "fact-resolve" });
        const resolution = await resolveFactTension({
          theme,
          issue,
          positionA,
          positionB,
          listExternalVoices,
          llm: factLlm,
          model: judgeModel,
          warn,
        });
        tension.status = resolution.status;
        tension.resolutionNote = resolution.note;
        updateTensionStatus(tension.id, resolution.status, resolution.note);
        const factText =
          resolution.status === "resolved"
            ? `事実確認の結果です: ${resolution.note}`
            : `この論点の事実対立は照会でも解消できませんでした。未解決の事実問題として結論に明記します。`;
        await commit(makeRecord({ persona: facilitator, round, turn: ++turn, text: factText }));
      } else {
        // ── [4] 止揚 (生成 → 折衷ゲート → 敵対的批准 → 修正ループ ≤2) ──────
        const outcome = await runSynthesisLoop({
          issueTitle: issue.title,
          tension,
          positionA,
          positionB,
          personaNames,
          paperSystem,
          genLlm: withCostLog(llm, { flow, sessionId, round, location: "synthesize" }),
          gateLlm: withCostLog(llm, { flow, sessionId, round, location: "elevation-gate" }),
          ratifyLlm: withCostLog(llm, { flow, sessionId, round, location: "ratify" }),
          judgeModel,
          warn,
        });
        syntheses.push(outcome.record);
        tension.status = outcome.tensionStatus;
        updateTensionStatus(tension.id, outcome.tensionStatus);
        const label =
          outcome.tensionStatus === "synthesized"
            ? "止揚 (批准済み)"
            : outcome.tensionStatus === "compromised"
              ? "折衷 (批准済み・止揚ではない)"
              : "合意不能 (両論併記)";
        const synthRecord = makeRecord({
          persona: facilitator,
          round,
          turn: ++turn,
          text: `${label}: ${outcome.record.text}`,
          act: "synthesize",
        });
        await commit(synthRecord);
        graph.apply({
          id: synthRecord.id,
          personaId: facilitator.id,
          act: "synthesize",
          targetId: null,
          turn: ++globalTurn,
        });
        // 折衷は止揚ストックに数えない (dialectic.md §4)。
        if (outcome.tensionStatus === "synthesized") ratifiedAufheben.push(outcome.record.text);
      }
    } else {
      log(`論点 ${round}: 片側が根拠を全て譲歩 — Tension を立てず決着`);
    }

    updateIssueStatus(issue.id, "concluded");
    issue.status = "concluded";
    const settlement: IssueSettlement = { issue, positions, tensions, syntheses };
    settlements.push(settlement);

    // ── issue 決着ごとのレンズ投票 (03 流用。winnerShare が収束シグナル (b)) ──
    const candidates = allUtterances
      .filter((u) => u.round === round && !u.isError && u.role !== "facilitator")
      .map((u) => ({ id: u.id, personaName: u.personaName, text: u.text }));
    const evaluation: VoteResult =
      candidates.length > 0
        ? await runRoundVote({
            theme,
            sessionId,
            round,
            voterCount: cfg.flow.voterCount,
            utterances: candidates,
            llm,
            lenses: cfg.flow.voteLenses,
            rng: rng as Rng,
            warn,
          })
        : { kind: "votes", tally: {}, winner: null, winnerShare: null };
    voteResults.push(evaluation);
    if (options.onVote) {
      try {
        await options.onVote({
          round,
          kind: evaluation.kind ?? "votes",
          tally: evaluation.tally,
          winner: evaluation.winner,
          candidates: candidates.map((c) => ({ id: c.id, personaName: c.personaName })),
        });
      } catch (e) {
        warn(`論点 ${round} onVote 通知失敗: ${(e as Error).message}`);
      }
    }

    // ── ペーパーライブ更新 (issue 決着表を「議論の経過」として焼き直す) ─────
    paper.rounds.push(settlementToRoundSummary(settlement, personaNames, evaluation.winnerShare ?? null));
    try {
      updatePaperBody(paperId, renderProgressMarkdown(paper.bodyMd ?? "", paper.rounds));
    } catch (e) {
      warn(`ペーパー更新失敗 (議論は続行): ${(e as Error).message}`);
    }

    // ── [5] 計測型収束 (コードのみ・2/3 + aufhebungTarget 上限キャップ) ──────
    const verdict = assessConvergence({
      unresolvedRebuttals: graph.unresolvedRebuttals().length,
      winnerShare: evaluation.winnerShare ?? null,
      convergeShare: cfg.flow.convergeShare,
      newClaimsInWindow: graph.newClaimsSince(globalTurn - Math.max(1, cfg.flow.dialectic.staleTurns)),
      ratifiedAufhebenCount: ratifiedAufheben.length,
      aufhebungTarget: cfg.facilitator.aufhebungTarget,
    });
    log(`収束判定: ${verdict.note}`);
    if (verdict.converged && settlements.length < issues.length) {
      log(`計測型収束シグナル ${verdict.met}/3${verdict.capReached ? " (上限キャップ)" : ""} — 残り論点を打ち切り`);
      convergedEarly = true;
    }
  }

  // ── 結論 (issue 決着表 + 未解決の事実問題 + 投票集中度) ────────────────────
  log("結論生成中...");
  const conclusionResult = await concludeDialectic({
    theme,
    sessionId,
    paperId,
    flow,
    settlements,
    personaNames,
    allUtterances,
    voteResults,
    ratifiedAufheben,
    llm,
    debatabilityNote:
      override?.debatability && !override.debatability.debatable
        ? `議論適性: 低 (両論武装可能な争点 ${override.debatability.armableBothCount} 件)`
        : undefined,
    warn,
  });
  log(`結論: ${conclusionResult.concluded ? conclusionResult.summary.slice(0, 80) : "結論なし"}`);

  // ペーパー最終焼き込み (経過 + 結論)。
  try {
    const rounds: RoundSummary[] = paper.rounds;
    updatePaperBody(paperId, renderProgressMarkdown(paper.bodyMd ?? "", rounds, conclusionResult.summary));
  } catch (e) {
    warn(`ペーパー最終更新失敗: ${(e as Error).message}`);
  }

  return {
    sessionId,
    paperId,
    utterances: allUtterances,
    rounds: settlements.length,
    conclusion: conclusionResult.summary,
    concluded: conclusionResult.concluded,
  };
}

/** dispatch 用: dialectic エンジンの議論フロー。 */
export async function runDialecticDiscussionFlow(
  theme: string,
  tags: readonly FlowTag[],
  deps: FlowRunOptions
): Promise<FlowDirectorResult> {
  return runDialecticFlow(theme, tags, { ...deps, flow: "discussion" });
}

/** dispatch 用: dialectic エンジンの改善フロー (決着はレンズ投票 — 06 §2)。 */
export async function runDialecticImprovementFlow(
  theme: string,
  tags: readonly FlowTag[],
  deps: FlowRunOptions
): Promise<FlowDirectorResult> {
  return runDialecticFlow(theme, tags, { ...deps, flow: "improvement" });
}
