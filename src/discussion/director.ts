/**
 * DiscussionDirector — 1 議論の進行オーケストレータ。
 *
 * - パーティ編成 (composition) → ターン進行 → 予算到達で続行/停止を問う →
 *   続行なら半数入替して次ラウンド、停止/タイムアウトなら司会がまとめて終了。
 * - LLM 呼び出し / Discord 投稿 / 続行ボタンは依存注入 (オフラインで test 可能)。
 * - 賛否はターン時に decideStance() で振る。
 */

import {
  formParty,
  swapHalf,
  decideStance,
  type PartyConfig,
  type PartyMember,
  type Rng,
} from "./composition.js";

export interface DirectorLLM {
  invoke(args: { system: string; prompt: string; model: string }): Promise<
    { ok: true; text: string } | { ok: false; error: string }
  >;
}

export interface DirectorDeps {
  llm: DirectorLLM;
  /** 発話を出力先 (Discord 等) に流す。 */
  postUtterance(member: PartyMember, text: string): Promise<void>;
  /**
   * 予算到達時に「続ける / 止める」を問う。true=続ける。
   * 実装は Discord ボタン interaction (無応答タイムアウトは false)。
   */
  askContinue(ctx: { round: number; total: number }): Promise<boolean>;
  /** 終了時の司会まとめを流す (任意)。 */
  postSummary?(member: PartyMember, text: string): Promise<void>;
  log: (msg: string) => void;
  rng: Rng;
  /** ターン間ディレイ ms (既定 0、live では数秒)。 */
  turnDelayMs?: number;
  /** ラウンド上限 (暴走ガード、既定 5)。 */
  maxRounds?: number;
}

export interface DiscussionSpec {
  topic: string;
  partyConfig: PartyConfig;
}

export interface DiscussionResult {
  utterances: Array<{ slotId: string; personaName: string; text: string }>;
  rounds: number;
  stoppedBy: "user-stop" | "max-rounds";
}

const delay = (ms: number) => new Promise<void>((r) => setTimeout(r, ms));

function roleLabel(m: PartyMember): string {
  return m.tier === "facilitator" ? "司会" : m.tier === "keyman" ? "キーマン" : "意見";
}

function buildTurnPrompt(
  topic: string,
  m: PartyMember,
  stance: string,
  recent: Array<{ personaName: string; text: string }>
): { system: string; prompt: string } {
  const stanceLine =
    m.tier === "facilitator"
      ? "あなたは司会。対立を整理し、止揚 (アウフヘーベン) で論点を一つに絞り、必要なら人間に問いかけて巻き込む。"
      : stance === "pro"
        ? "このターンのあなたの立場は【賛成寄り】。主張を筋の通った形で擁護・補強する。"
        : "このターンのあなたの立場は【否定寄り】。健全な反論・反例・見落とされた弱点を投げる。";
  const system = [
    `あなたは議論ペルソナ「${m.personaName}」(${roleLabel(m)})。`,
    `特徴: ${m.traits.join(" / ")} / 話し方: ${m.speechStyle}`,
    stanceLine,
    "Discord のチャットで実在の人間が話すように、自然な口語で 1〜2 文だけ書く。",
    "ラベルや箇条書きは使わない。既出の繰り返しは避け、議論を一歩進める。",
    "直近の発言と同じ結論・同じ理由の言い換えは禁止。重複しそうなら、未検討のユーザ層、メカニクス条件、反例、判断基準のいずれかを新しく出す。",
    "合意できない場合も同じ賛否を往復せず、未決条件、検証方法、または小さな修正案に分解する。",
    "発言テキストのみを返す (JSON や前置きは不要)。付け足す事が無ければ空行のみ返す。",
  ].join("\n");
  const history = recent.length
    ? recent.map((u) => `${u.personaName}: ${u.text}`).join("\n")
    : "(まだ発言なし)";
  const prompt = [`# 議題\n${topic}`, "", `# 直近の発言\n${history}`, "", "あなたの発言:"].join("\n");
  return { system, prompt };
}

/** 次に喋るメンバーを決める。意見/キーマンを巡回し、数ターンごとに司会を挟む。 */
function pickSpeaker(party: PartyMember[], turnInRound: number): PartyMember {
  const facilitator = party.find((m) => m.tier === "facilitator")!;
  const speakers = party.filter((m) => m.tier !== "facilitator");
  // 4 ターンに 1 回 (ただし冒頭は除く) 司会を入れる。
  if (turnInRound > 0 && turnInRound % 4 === 3) return facilitator;
  return speakers[turnInRound % speakers.length];
}

export async function runDiscussion(spec: DiscussionSpec, deps: DirectorDeps): Promise<DiscussionResult> {
  const maxRounds = deps.maxRounds ?? 5;
  const budget = spec.partyConfig.expectedUtterances;
  let party = formParty(spec.partyConfig, deps.rng);
  deps.log(`party formed: ${party.map((m) => `${m.personaName}(${roleLabel(m)}/${m.model})`).join(", ")}`);

  const utterances: DiscussionResult["utterances"] = [];
  let round = 0;
  let stoppedBy: DiscussionResult["stoppedBy"] = "max-rounds";

  while (round < maxRounds) {
    let spokenThisRound = 0;
    let turnInRound = 0;
    while (spokenThisRound < budget) {
      const m = pickSpeaker(party, turnInRound);
      turnInRound++;
      const stance = decideStance(m, deps.rng);
      const recent = utterances.slice(-10);
      const { system, prompt } = buildTurnPrompt(spec.topic, m, stance, recent);
      const res = await deps.llm.invoke({ system, prompt, model: m.model });
      if (!res.ok) {
        deps.log(`turn skip (llm error: ${res.error})`);
        continue;
      }
      const text = res.text.trim();
      if (text.length === 0) {
        deps.log(`turn skip (${m.personaName}: 空応答)`);
        continue;
      }
      utterances.push({ slotId: m.slotId, personaName: m.personaName, text });
      await deps.postUtterance(m, text);
      spokenThisRound++;
      if (deps.turnDelayMs) await delay(deps.turnDelayMs);
    }

    // 予算到達 → 続行 / 停止
    const cont = await deps.askContinue({ round: round + 1, total: utterances.length });
    if (!cont) {
      stoppedBy = "user-stop";
      break;
    }
    party = swapHalf(party, spec.partyConfig, deps.rng);
    deps.log(`round ${round + 1} → swap: ${party.map((m) => m.personaName).join(", ")}`);
    round++;
  }

  // 司会のまとめ
  if (deps.postSummary) {
    const facilitator = party.find((m) => m.tier === "facilitator")!;
    const recent = utterances.slice(-12);
    const { system, prompt } = buildTurnPrompt(
      `${spec.topic}\n(これは議論の締め。ここまでの論点を 2〜3 文で要約し、到達した結論または残った論点を述べる)`,
      facilitator,
      "neutral",
      recent
    );
    const res = await deps.llm.invoke({ system, prompt, model: facilitator.model });
    if (res.ok && res.text.trim()) await deps.postSummary(facilitator, res.text.trim());
  }

  return { utterances, rounds: round + 1, stoppedBy };
}
