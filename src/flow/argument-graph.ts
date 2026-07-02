/**
 * 論証グラフ (respec 05 §3 / dialectic.md §0)。
 *
 * dialectic エンジンの発話 (act/target 付き) から「誰がどの主張に反論し、何が未応答か」を
 * 決定的に導出する。**純関数 + 小さな状態クラス・LLM 不使用**。
 * グラフはセッションメモリで構築し、収束判定・スケジューラ・批准入力の 3 箇所が読む。
 * 永続は flow_utterance (act/target_id) からいつでも再構築できる (別テーブル不要)。
 *
 * 遷移規則 (claim/ground 単位):
 *   claim ── rebut ──> challenged ── support(同陣営, target=rebut) ──> defended
 *                                 └─ concede(被反論側) ─────────────> conceded
 */

/** 会話行為 (dialogue act, respec 05 §1)。 */
export type DialogueAct = "claim" | "support" | "rebut" | "concede" | "question" | "synthesize";

const ACTS: readonly DialogueAct[] = ["claim", "support", "rebut", "concede", "question", "synthesize"];

/**
 * DB / LLM 由来の act 文字列を DialogueAct に正規化する。
 * null / 未知値は claim 扱い (後方互換: act 列なしの旧行・rounds エンジンの発話)。
 */
export function coerceAct(act: string | null | undefined): DialogueAct {
  return (ACTS as readonly string[]).includes(act ?? "") ? (act as DialogueAct) : "claim";
}

/** グラフに適用する発話 (flow_utterance の射影)。 */
export interface GraphUtterance {
  id: string;
  personaId: string;
  act: DialogueAct;
  /** 応答先 utterance id (claim/question は null 可)。 */
  targetId: string | null;
  /** セッション通し のターン番号 (newClaimsSince 用)。 */
  turn: number;
}

/** rebut 辺の状態。challenged = 未応答 (defend も concede もされていない)。 */
export type RebuttalState = "challenged" | "defended" | "conceded";

export interface RebuttalEdge {
  /** rebut 発話の id。 */
  rebutId: string;
  /** 反論された claim 発話の id。 */
  targetId: string;
  /** 反論した persona。 */
  byPersonaId: string;
  /** 反論された claim の持ち主 (= 応答義務を負う persona)。ターゲット不明なら null。 */
  targetPersonaId: string | null;
  state: RebuttalState;
}

export interface ConcessionEdge {
  /** concede 発話の id。 */
  concedeId: string;
  /** 譲歩の対象になった rebut 発話の id。 */
  rebutId: string;
  /** 元の claim 発話の id (止揚の原材料)。 */
  targetId: string;
}

interface ClaimNode {
  id: string;
  personaId: string;
  turn: number;
  /** この claim を対象にした発話 (rebut/support/concede/question) が 1 つでもあるか。 */
  touched: boolean;
}

/**
 * 論証グラフ。apply で状態遷移し、計測メソッドで収束判定・スケジューラの材料を返す。
 * 決定的 (入力順のみに依存)。
 */
export class ArgumentGraph {
  private readonly claims = new Map<string, ClaimNode>();
  /** rebutId → edge。 */
  private readonly rebuttals = new Map<string, RebuttalEdge>();
  private readonly conceded: ConcessionEdge[] = [];

  /** 発話を 1 つ適用し状態遷移する (未知 target は無視して受理 = degrade)。 */
  apply(u: GraphUtterance): void {
    switch (u.act) {
      case "claim":
      case "synthesize": {
        // synthesize も「新しい主張ノード」としては claim と同じ扱い (反論可能)。
        this.claims.set(u.id, { id: u.id, personaId: u.personaId, turn: u.turn, touched: false });
        if (u.targetId) this.touch(u.targetId);
        return;
      }
      case "rebut": {
        if (!u.targetId) return; // 対象なし rebut は論証上の効果なし (発話は残る)
        this.touch(u.targetId);
        const target = this.claims.get(u.targetId);
        this.rebuttals.set(u.id, {
          rebutId: u.id,
          targetId: u.targetId,
          byPersonaId: u.personaId,
          targetPersonaId: target?.personaId ?? null,
          state: "challenged",
        });
        return;
      }
      case "support": {
        if (!u.targetId) return;
        const edge = this.rebuttals.get(u.targetId);
        if (edge) {
          // 相手の rebut への応答 = 防御 (spec: support(同陣営, target=rebut) → defended)。
          // 反論者自身の support は防御にならない (自分の反論を自分で「守る」のは無意味)。
          if (edge.state === "challenged" && u.personaId !== edge.byPersonaId) {
            edge.state = "defended";
          }
        } else {
          // claim への support = 補強 (touched のみ)。
          this.touch(u.targetId);
        }
        return;
      }
      case "concede": {
        if (!u.targetId) return;
        const edge = this.rebuttals.get(u.targetId);
        if (edge) {
          if (edge.state === "challenged") {
            edge.state = "conceded";
            this.conceded.push({ concedeId: u.id, rebutId: edge.rebutId, targetId: edge.targetId });
          }
        } else {
          // claim を直接 concede した場合: その claim への未応答 rebut を全部 conceded にする。
          this.touch(u.targetId);
          for (const e of this.rebuttals.values()) {
            if (e.targetId === u.targetId && e.state === "challenged") {
              e.state = "conceded";
              this.conceded.push({ concedeId: u.id, rebutId: e.rebutId, targetId: e.targetId });
            }
          }
        }
        return;
      }
      case "question": {
        if (u.targetId) this.touch(u.targetId);
        return;
      }
    }
  }

  private touch(targetId: string): void {
    const node = this.claims.get(targetId);
    if (node) node.touched = true;
  }

  /** rebut されて defend も concede もされていない辺 (未応答反論)。挿入順。 */
  unresolvedRebuttals(): RebuttalEdge[] {
    return [...this.rebuttals.values()].filter((e) => e.state === "challenged");
  }

  /** 誰も触れていない claim の id 列 (挿入順)。 */
  unansweredClaims(): string[] {
    return [...this.claims.values()].filter((c) => !c.touched).map((c) => c.id);
  }

  /** concede 済みの辺 (止揚の原材料)。 */
  concessions(): ConcessionEdge[] {
    return [...this.conceded];
  }

  /** turn より後の新規 claim 数 (新規論点の枯渇判定用)。 */
  newClaimsSince(turn: number): number {
    let n = 0;
    for (const c of this.claims.values()) {
      if (c.turn > turn) n++;
    }
    return n;
  }

  /** persona ごとの claim 保有数など、スケジューラの接続数計測に使う claim 一覧。 */
  claimNodes(): ReadonlyArray<{ id: string; personaId: string; turn: number }> {
    return [...this.claims.values()].map((c) => ({ id: c.id, personaId: c.personaId, turn: c.turn }));
  }

  /** claim を対象にした rebut 辺の数 (状態問わず)。スケジューラの「接続数最大」計測用。 */
  edgeCountFor(claimId: string): number {
    let n = 0;
    for (const e of this.rebuttals.values()) {
      if (e.targetId === claimId) n++;
    }
    return n;
  }

  /** claim を対象にした rebut 辺 (状態問わず・挿入順)。ground 状態の write-through 同期用。 */
  rebuttalsTargeting(claimId: string): RebuttalEdge[] {
    return [...this.rebuttals.values()].filter((e) => e.targetId === claimId);
  }

  /** rebut 辺を id で引く (concede/support の対象解決用)。 */
  rebuttalById(rebutId: string): RebuttalEdge | undefined {
    return this.rebuttals.get(rebutId);
  }
}

/**
 * flow_utterance 行 (act 列は NULL 可) からグラフを再構築する (後方互換 read 経路)。
 * act=NULL の旧行は claim 扱い (respec 05 §2)。
 */
export function buildGraphFromRows(
  rows: ReadonlyArray<{
    id: string;
    persona_id: string;
    act: string | null;
    target_id: string | null;
    turn: number;
  }>
): ArgumentGraph {
  const g = new ArgumentGraph();
  for (const r of rows) {
    g.apply({
      id: r.id,
      personaId: r.persona_id,
      act: coerceAct(r.act),
      targetId: r.target_id,
      turn: r.turn,
    });
  }
  return g;
}
