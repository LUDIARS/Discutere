/**
 * 論拠の応酬 [2] の進行 (dialectic.md §2.6) — driver から 1 論点ぶんを切り出した進行台本。
 *
 *   1. 応答待ちの論拠を幅優先で 1 件取り出す (exchange.ts)
 *   2. 相手陣営の発言数最少ペルソナが「反論 (論拠 1 件)」か「認める」で答える
 *   3. 反論は前提ルールのゲート (rule-check.ts) を通ったときだけ論拠として積む
 *   4. 応答待ちが無くなる (= 論拠が落ち着く) か応答ターン上限で終わる
 * 終わったら定立の論拠の決着を Position の根拠状態へ書き戻す (goal.ts の論破判定が読む)。
 *
 * 永続・通知・論証グラフの更新は driver から渡された commit / applyGraph に任せる。
 */

import type { LLMClient } from "../../persona-engine/llm/client.js";
import type { DialogueAct } from "../argument-graph.js";
import type { FlowUtteranceRecord } from "../director.js";
import { personaStance, type FlowPersona } from "../personas.js";
import { GroundExchange, opposingSide, type ArgumentNode, type Side } from "./exchange.js";
import { buildExchangePrompt, parseExchangeResponse } from "./exchange-prompt.js";
import { gateRebut, judgeConcession, renderFoulNotice } from "./rule-check.js";
import { renderFacilitatorLine, renderNomination } from "./scheduler.js";
import { updatePositionGrounds, type IssueRecord, type PositionRecord } from "./store.js";
import { attributionSuffix, checkAttribution, type ClaimKind } from "../proxy/attribution.js";
import type { UtteranceDigest } from "./turn-prompt.js";

export interface RecordArgs {
  persona: FlowPersona;
  round: number;
  turn: number;
  text: string;
  act?: DialogueAct;
  targetId?: string;
  isError?: boolean;
  /** 代理議論のみ: 検査済みの帰属区分と根拠 id。 */
  claimKind?: ClaimKind;
  evidenceIds?: string[];
}

export interface GroundExchangeArgs {
  issue: IssueRecord;
  round: number;
  /** 定立の投稿まで終えた時点のターン番号 (この続きから数える)。 */
  turn: number;
  exchange: GroundExchange;
  positionBySide: ReadonlyMap<Side, PositionRecord>;
  speakers: readonly FlowPersona[];
  /** 陣営に発言者がいないときの代表 (ensureDebaters の pro/con)。 */
  debaters: Readonly<Record<Side, FlowPersona>>;
  facilitator: FlowPersona;
  personaNames: ReadonlyMap<string, string>;
  utteranceIndex: ReadonlyMap<string, UtteranceDigest>;
  speakCounts: ReadonlyMap<string, number>;
  paperSystem: string;
  voicesBlock: string;
  /** 応答ターン上限 (flow.dialectic.exchangeTurns)。 */
  turnCap: number;
  ruleCheck: boolean;
  judgeModel?: string;
  /** 発言者・ターンごとの withCostLog 済み LLM。 */
  turnLlm: (persona: FlowPersona, turn: number) => LLMClient;
  ruleCheckLlm: (turn: number) => LLMClient;
  facilitatorLlm: LLMClient;
  makeRecord: (args: RecordArgs) => FlowUtteranceRecord;
  commit: (record: FlowUtteranceRecord) => Promise<void>;
  applyGraph: (record: FlowUtteranceRecord, act: DialogueAct, targetId: string | null) => void;
  log: (msg: string) => void;
  warn: (msg: string) => void;
}

/** 陣営の発言者 (発言数最少・同点は入力順)。 */
export function pickSideSpeaker(
  speakers: readonly FlowPersona[],
  side: Side,
  speakCounts: ReadonlyMap<string, number>
): FlowPersona | null {
  let best: FlowPersona | null = null;
  let bestCount = Infinity;
  for (const p of speakers) {
    if (personaStance(p) !== side) continue;
    const c = speakCounts.get(p.id) ?? 0;
    if (c < bestCount) {
      best = p;
      bestCount = c;
    }
  }
  return best;
}

/** 反論先ノードをゲート用の Position に見立てる (定立の論拠は元の Position、反論はその 1 論拠だけの主張)。 */
function gateTarget(node: ArgumentNode, positionBySide: ReadonlyMap<Side, PositionRecord>): {
  position: PositionRecord;
  groundId: string;
} | null {
  const owner = positionBySide.get(node.side);
  if (!owner) return null;
  if (node.depth === 0 && node.groundId) return { position: owner, groundId: node.groundId };
  return {
    position: {
      ...owner,
      claim: node.text,
      qualifier: owner.qualifier,
      grounds: [{ id: node.id, text: node.data, state: "unchallenged", ...(node.warrant ? { warrant: node.warrant } : {}), ...(node.kind ? { kind: node.kind } : {}) }],
    },
    groundId: node.id,
  };
}

/** 応酬を回し、最後のターン番号を返す。 */
export async function runGroundExchange(args: GroundExchangeArgs): Promise<number> {
  const { issue, round, exchange, log, warn } = args;
  let turn = args.turn;
  let answered = 0;

  for (let node = exchange.next(); node; node = exchange.next()) {
    if (answered >= args.turnCap) {
      const cut = exchange.cutRemaining();
      log(`論点 ${round}: 応答ターン上限 ${args.turnCap} — 応答待ち ${cut} 件を打ち切り (決着つかずとして扱う)`);
      break;
    }
    answered++;
    const side = opposingSide(node.side);
    const persona = pickSideSpeaker(args.speakers, side, args.speakCounts) ?? args.debaters[side];
    turn += 1;

    // 反論への再反論は進行役が指名する (定立の論拠への反論は投げ合いの続きなので指名しない)。
    if (node.depth > 0) {
      const nomination = await renderNomination({
        decision: {
          kind: "respond",
          personaId: persona.id,
          targetUtteranceId: node.utteranceId,
          allowedActs: ["rebut", "concede"],
          reason: "反論への応答",
        },
        personaName: persona.name,
        target: args.utteranceIndex.get(node.utteranceId),
        llm: args.facilitatorLlm,
        model: args.facilitator.model,
        warn,
      });
      await args.commit(args.makeRecord({ persona: args.facilitator, round, turn, text: nomination }));
    }

    const prompt = buildExchangePrompt({
      persona,
      stance: personaStance(persona),
      issue,
      positions: [...args.positionBySide.values()],
      personaNames: args.personaNames,
      path: exchange.path(node.id),
      voicesBlock: args.voicesBlock,
    });
    const result = await args.turnLlm(persona, turn).invoke({ system: args.paperSystem, prompt, model: persona.model });
    if (!result.ok) {
      warn(`論点 ${round} ターン ${turn} (${persona.name}) LLM エラー: ${result.error} — 応答なしとして扱う`);
      await args.commit(args.makeRecord({ persona, round, turn, text: `[エラー: ${result.error}]`, isError: true }));
      exchange.fail(node.id);
      continue;
    }

    const parsed = parseExchangeResponse(result.text, warn);
    if (!parsed.text) {
      log(`論点 ${round} ターン ${turn} (${persona.name}): 空応答 — 応答なしとして扱う`);
      exchange.fail(node.id);
      continue;
    }

    // 「認める」の申告が中身と合っているかを確かめる (追撃を「認める」に数えない)。
    // 中身が反論なら、論拠 (データ・論拠) を伴わない問いとして扱い相手の論拠は成り立ったままにする。
    if (parsed.act === "concede" && args.ruleCheck) {
      const verdict = await judgeConcession({
        targetText: node.text,
        replyText: parsed.text,
        llm: args.ruleCheckLlm(turn),
        model: args.judgeModel,
        warn,
      });
      if (verdict === "rebut") {
        warn(`論点 ${round} ターン ${turn} (${persona.name}): 「認める」の申告だが中身は反論 — question として扱う`);
        const record = args.makeRecord({ persona, round, turn, text: parsed.text, act: "question", targetId: node.utteranceId });
        await args.commit(record);
        args.applyGraph(record, "question", node.utteranceId);
        exchange.fail(node.id);
        continue;
      }
    }

    if (parsed.act === "concede") {
      const record = args.makeRecord({ persona, round, turn, text: parsed.text, act: "concede", targetId: node.utteranceId });
      await args.commit(record);
      args.applyGraph(record, "concede", node.utteranceId);
      exchange.accept(node.id);
      continue;
    }

    // 代理議論: 本人の発言と申告した反論は、束の根拠 id と原文で裏付けられていなければ論拠にしない。
    const attribution =
      persona.proxy && parsed.act === "rebut"
        ? checkAttribution({
            claimKind: parsed.claimKind,
            evidenceIds: parsed.evidenceIds,
            data: parsed.data ?? "",
            scope: persona.proxy,
          })
        : null;
    if (attribution && !attribution.ok) {
      warn(`論点 ${round} ターン ${turn} (${persona.name}): 帰属の検査に通らない (${attribution.foul}) — question に格下げ`);
      const record = args.makeRecord({ persona, round, turn, text: parsed.text, act: "question", targetId: node.utteranceId });
      await args.commit(record);
      args.applyGraph(record, "question", node.utteranceId);
      exchange.fail(node.id);
      await args.commit(
        args.makeRecord({ persona: args.facilitator, round, turn: ++turn, text: renderFoulNotice(persona.name, attribution.foul) })
      );
      continue;
    }

    // 前提ルールのゲート (dialectic.md §2.5): 反論は論拠 1 件 (データ + 論拠) で、突く要素を宣言する。
    const target = gateTarget(node, args.positionBySide);
    const gate =
      parsed.act === "rebut" && target
        ? await gateRebut({
            issue,
            position: target.position,
            // 立場の取り違え (自分の陣営と逆向きの反論) を判定するため、応答者の陣営の主張を渡す。
            ownPosition: args.positionBySide.get(side) ?? null,
            attack: parsed.attack,
            groundId: target.groundId,
            data: parsed.data,
            warrant: parsed.warrant,
            rebutText: parsed.text,
            judge: args.ruleCheck,
            llm: args.ruleCheckLlm(turn),
            model: args.judgeModel,
            warn,
          })
        : null;

    if (gate?.accepted && parsed.data) {
      // 代理の反論は露出文に帰属の印 ([E2] / （推測）) を付け、区分と根拠 id を発話に残す。
      const proxyMark = attribution?.ok
        ? {
            text: `${parsed.text}${attributionSuffix(attribution.claimKind, attribution.evidenceIds)}`,
            claimKind: attribution.claimKind,
            evidenceIds: attribution.evidenceIds,
          }
        : null;
      const record = args.makeRecord({
        persona,
        round,
        turn,
        text: proxyMark?.text ?? parsed.text,
        act: "rebut",
        targetId: node.utteranceId,
        ...(proxyMark ? { claimKind: proxyMark.claimKind, evidenceIds: proxyMark.evidenceIds } : {}),
      });
      await args.commit(record);
      args.applyGraph(record, "rebut", node.utteranceId);
      exchange.rebut(node.id, {
        utteranceId: record.id,
        side,
        personaId: persona.id,
        groundId: null,
        data: parsed.data,
        ...(parsed.warrant ? { warrant: parsed.warrant } : {}),
        ...(parsed.kind ? { kind: parsed.kind } : {}),
        text: parsed.text,
      });
      continue;
    }

    // 有効な反論にならなかった: 発言は問いとして残し、相手の論拠は成り立ったまま。
    const record = args.makeRecord({ persona, round, turn, text: parsed.text, act: "question", targetId: node.utteranceId });
    await args.commit(record);
    args.applyGraph(record, "question", node.utteranceId);
    exchange.fail(node.id);
    if (gate && !gate.accepted) {
      warn(`論点 ${round} ターン ${turn} (${persona.name}): 前提ルール違反 (${gate.foul}) — question に格下げ`);
      await args.commit(
        args.makeRecord({ persona: args.facilitator, round, turn: ++turn, text: renderFoulNotice(persona.name, gate.foul) })
      );
    }
  }

  writeBackGroundStates(exchange, args.positionBySide);

  // 論拠が落ち着いたら進行役がまとめに入る。
  const pro = exchange.standingCount("pro");
  const con = exchange.standingCount("con");
  const closing = await renderFacilitatorLine({
    content:
      `論拠のやり取りが落ち着きました。崩れずに残った論拠は、賛成側 ${pro.standing}/${pro.total} 件、` +
      `反対側 ${con.standing}/${con.total} 件です。ここまでをまとめます。`,
    llm: args.facilitatorLlm,
    model: args.facilitator.model,
    warn,
  });
  await args.commit(args.makeRecord({ persona: args.facilitator, round, turn: ++turn, text: closing }));
  log(`論点 ${round}: 論拠の応酬 ${answered} ターンで終了 (賛成 ${pro.standing}/${pro.total} / 反対 ${con.standing}/${con.total})`);
  return turn;
}

/** 定立の論拠の決着を Position の根拠状態へ書き戻す (変化した Position だけ永続)。 */
export function writeBackGroundStates(
  exchange: GroundExchange,
  positionBySide: ReadonlyMap<Side, PositionRecord>
): void {
  const states = exchange.groundStates();
  for (const position of positionBySide.values()) {
    let touched = false;
    for (const g of position.grounds) {
      const next = states.get(g.id);
      if (next && g.state !== next) {
        g.state = next;
        touched = true;
      }
    }
    if (touched) updatePositionGrounds(position.id, position.grounds);
  }
}
