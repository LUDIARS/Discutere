/**
 * 論拠の応酬 [2] (dialectic.md §2.6) — 決定的コード・LLM 不使用。
 *
 * 論拠 (トゥールミンの data + warrant) を 1 件 = 1 ノードとして持ち、反論もまた論拠 1 件の
 * ノードとして親の論拠にぶら下げる。応答待ちのノードは幅優先のキューに積み、
 *   1 段目: 定立で投げ合った論拠すべて
 *   2 段目: 1 段目への反論すべて
 *   ...
 * の順に相手陣営が答える (どちらかの論拠だけが後回しにならない)。
 * 答え方は「反論 (論拠 1 件)」か「認める」。キューが空になったら応酬が落ち着いたとみなす。
 *
 * 決着の評価 (三値): ノードは
 *   - standing  : 有効な反論が 1 つも成り立っていない
 *   - refuted   : 成り立った (standing の) 反論がある
 *   - contested : 上限で打ち切られ、決着がつかなかった
 * 打ち切りは最後に答えた側を勝たせないため contested とし、根拠を崩さない。
 */

import type { InferenceKind } from "./premise-rules.js";
import type { GroundState } from "./store.js";

export type Side = "pro" | "con";

export type NodeStatus =
  /** 応答待ち (キューにある)。 */
  | "open"
  /** 相手が答えた (反論した / 答えたが有効な反論にならなかった)。 */
  | "answered"
  /** 相手が認めた (このノードは確定で成り立つ)。 */
  | "accepted"
  /** 深さ・ターン上限で相手の応答を待たずに打ち切られた。 */
  | "cut";

export interface ArgumentNode {
  id: string;
  /** このノードを投稿した発話 id (反論の target / 論証グラフの接続先)。 */
  utteranceId: string;
  side: Side;
  personaId: string;
  /** 反論先ノード。定立の論拠は null。 */
  parentId: string | null;
  /** 定立の論拠 = 0。反論ごとに +1。 */
  depth: number;
  /** 定立の論拠なら Position の根拠 id (pro-G1 等)。反論は null。 */
  groundId: string | null;
  data: string;
  warrant?: string;
  kind?: InferenceKind;
  /** 露出した口語文。 */
  text: string;
  status: NodeStatus;
}

export type NodeVerdict = "standing" | "refuted" | "contested";

export interface ExchangeOptions {
  /** 反論の深さ上限 (定立の論拠 = 0)。この深さのノードには答えさせない。 */
  maxDepth: number;
}

export type NewNode = Omit<ArgumentNode, "id" | "depth" | "status" | "parentId">;

/** 応酬の状態。driver が 1 論点につき 1 つ持つ。 */
export class GroundExchange {
  private readonly nodes = new Map<string, ArgumentNode>();
  private readonly queue: string[] = [];
  private seq = 0;

  constructor(private readonly options: ExchangeOptions) {}

  /** 定立の論拠を積む (投稿順 = 応答順)。 */
  addOpening(node: NewNode): ArgumentNode {
    return this.insert(node, null, 0);
  }

  /** 次に答えるべきノード (幅優先)。呼び出し側は rebut / accept / fail のどれかで閉じる。 */
  next(): ArgumentNode | null {
    while (this.queue.length > 0) {
      const node = this.nodes.get(this.queue[0]);
      if (node && node.status === "open") return node;
      this.queue.shift();
    }
    return null;
  }

  /** target への反論 (論拠 1 件) を登録する。深さ上限に達した反論は答えさせず打ち切る。 */
  rebut(targetId: string, node: NewNode): ArgumentNode {
    const target = this.require(targetId);
    this.close(target, "answered");
    return this.insert(node, target.id, target.depth + 1);
  }

  /** 応答者が target を認めた。 */
  accept(targetId: string): void {
    this.close(this.require(targetId), "accepted");
  }

  /** 応答が有効な反論にならなかった (LLM 障害・空応答・前提ルール違反)。target は成り立ったまま。 */
  fail(targetId: string): void {
    this.close(this.require(targetId), "answered");
  }

  /** ターン上限: 残りの応答待ちをすべて打ち切る。打ち切った件数を返す。 */
  cutRemaining(): number {
    let n = 0;
    for (const id of this.queue) {
      const node = this.nodes.get(id);
      if (node && node.status === "open") {
        node.status = "cut";
        n++;
      }
    }
    this.queue.length = 0;
    return n;
  }

  /** 応答待ちが無い = 論拠が落ち着いた。 */
  settled(): boolean {
    return this.next() === null;
  }

  get(id: string): ArgumentNode | undefined {
    return this.nodes.get(id);
  }

  all(): ArgumentNode[] {
    return [...this.nodes.values()];
  }

  children(id: string): ArgumentNode[] {
    return this.all().filter((n) => n.parentId === id);
  }

  /** 定立の論拠から node までの経路 (文脈提示用・根が先頭)。 */
  path(id: string): ArgumentNode[] {
    const out: ArgumentNode[] = [];
    let cur = this.nodes.get(id);
    while (cur) {
      out.unshift(cur);
      cur = cur.parentId ? this.nodes.get(cur.parentId) : undefined;
    }
    return out;
  }

  /** ノードの決着 (三値・再帰)。 */
  verdict(id: string): NodeVerdict {
    const node = this.require(id);
    if (node.status === "cut") return "contested";
    let contested = false;
    for (const child of this.children(id)) {
      const v = this.verdict(child.id);
      if (v === "standing") return "refuted";
      if (v === "contested") contested = true;
    }
    return contested ? "contested" : "standing";
  }

  /**
   * 定立の論拠の決着を Position の根拠状態へ写す (goal.ts の論破判定が読む)。
   *   反論なし                         → unchallenged
   *   守り切った / 決着つかず           → defended
   *   崩れた: 持ち主が反論を認めた       → conceded
   *   崩れた: 答えられなかった・再反論が崩れた → challenged
   */
  groundStates(): Map<string, GroundState> {
    const out = new Map<string, GroundState>();
    for (const node of this.all()) {
      if (node.depth !== 0 || !node.groundId) continue;
      const kids = this.children(node.id);
      let state: GroundState;
      if (kids.length === 0) state = "unchallenged";
      else if (this.verdict(node.id) !== "refuted") state = "defended";
      else state = kids.some((k) => k.status === "accepted") ? "conceded" : "challenged";
      out.set(node.groundId, state);
    }
    return out;
  }

  /** 陣営ごとの「崩れていない定立の論拠」数 (進行役のまとめ文用)。 */
  standingCount(side: Side): { standing: number; total: number } {
    let standing = 0;
    let total = 0;
    for (const node of this.all()) {
      if (node.depth !== 0 || node.side !== side) continue;
      total++;
      if (this.verdict(node.id) !== "refuted") standing++;
    }
    return { standing, total };
  }

  private insert(node: NewNode, parentId: string | null, depth: number): ArgumentNode {
    const full: ArgumentNode = {
      ...node,
      id: `N${++this.seq}`,
      parentId,
      depth,
      status: depth >= this.options.maxDepth ? "cut" : "open",
    };
    this.nodes.set(full.id, full);
    if (full.status === "open") this.queue.push(full.id);
    return full;
  }

  private close(node: ArgumentNode, status: "answered" | "accepted"): void {
    if (node.status === "open") node.status = status;
  }

  private require(id: string): ArgumentNode {
    const node = this.nodes.get(id);
    if (!node) throw new Error(`unknown argument node: ${id}`);
    return node;
  }
}

/** 応答者の陣営 (ノードの相手側)。 */
export function opposingSide(side: Side): Side {
  return side === "pro" ? "con" : "pro";
}
