/**
 * 未来予測フェーズ (spec/feature/flow/foresight.md) の prompt 組み立て (pure)。
 *
 * イノベーション理論 (Christensen の破壊的イノベーション / プロダクトライフサイクル /
 * Rogers の普及理論) を土台に、
 *   1. 提案: 現行ライフサイクルを破壊可能な一手 + 実行時に起きること + リスク
 *   2. 議論: 改革派 (賛同) と 保守派 (否定) が提案と帰結を戦わせる
 *   3. 予測: その結果訪れる 3 / 5 / 10 年後 + ビジネススキームなら利益率・確度・ノウハウ強度
 * の 3 段の prompt を返す。LLM 呼び出しは phase.ts が行う。
 */

export type ForesightCamp = "reform" | "conservative";

/** 議論の材料 (本議論の結論 + 未来予測フェーズ内の発言)。 */
export interface ForesightLine {
  personaName: string;
  camp: ForesightCamp;
  text: string;
}

/** 未来予測は結論を変えない参考 (アドバイザリー) であることを全 prompt に入れる。 */
const ADVISORY_LINE =
  "これは本議論の結論とは別の参考 (アドバイザリー)。本議論の結論を覆したり書き換えたりしない。";

const THEORY_LINE =
  "道具にする理論: Christensen の破壊的イノベーション (ローエンド型 / 新市場型、性能の過剰供給、既存企業のジレンマ、価値網)、プロダクトライフサイクル (導入 / 成長 / 成熟 / 衰退)、Rogers の普及理論 (イノベーター〜ラガード、キャズム)。";

const CAMP_LABEL: Record<ForesightCamp, string> = {
  reform: "改革派",
  conservative: "保守派",
};

function conclusionBlock(conclusion: string): string {
  const text = conclusion.trim();
  return text ? `# ここまでの議論の結論\n${text}` : "# ここまでの議論の結論\n(結論なし)";
}

function linesBlock(lines: readonly ForesightLine[]): string {
  if (lines.length === 0) return "(まだ発言なし)";
  return lines.map((l) => `${l.personaName} [${CAMP_LABEL[l.camp]}]: ${l.text}`).join("\n");
}

/** 提案 prompt: 現行ライフサイクルを破壊可能な一手 + 起きること + リスク。 */
export function buildProposalPrompt(theme: string, conclusion: string): { system: string; prompt: string } {
  const system = [
    "あなたはイノベーション理論に通じた戦略アナリストで、議論の進行役。",
    THEORY_LINE,
    ADVISORY_LINE,
    "議題の対象について、現行のプロダクトライフサイクルを破壊可能な一手を 1 つ提案する。",
    "次の見出しで、日本語で簡潔に書く (全体で 900 字以内):",
    "【現行のライフサイクル】いまどの段階にあり、既存の主流が何を過剰に供給しているか",
    "【破壊可能な一手】何を、誰に向けて、どの足場から始めるか (具体的な一手 1 つ)",
    "【実行すると何が起きるか】顧客・既存企業・市場構造の順に、起きる変化",
    "【リスク】失敗の筋書き、既存企業の反撃、実行上の制約",
    "前置きや結びの挨拶は書かない。",
  ].join("\n");
  const prompt = [`# 議題\n${theme}`, "", conclusionBlock(conclusion), "", "提案:"].join("\n");
  return { system, prompt };
}

/** 陣営の立場行。 */
export function campStanceLine(camp: ForesightCamp): string {
  return camp === "reform"
    ? "あなたは【改革派 (賛同)】。提案された破壊的な一手を支持し、なぜ現行のライフサイクルを壊せるのか、実行で何が良く変わるのかを具体的に擁護する。リスクを指摘されたら、回避策か「それでも取るべき理由」で応える。"
    : "あなたは【保守派 (否定)】。提案された破壊的な一手に反対し、既存顧客・既存の価値網・実行コスト・失敗時の損害から現行路線を守る理由を示す。反対するだけでなく、提案が失敗する具体的な筋書きか、成立に必要な未達の条件を出す。";
}

/** 議論ターン prompt: 改革派 / 保守派として提案と帰結を論じる。 */
export function buildCampTurnPrompt(args: {
  theme: string;
  proposal: string;
  personaName: string;
  camp: ForesightCamp;
  lines: readonly ForesightLine[];
}): { system: string; prompt: string } {
  const system = [
    `あなたは議論の参加者「${args.personaName}」。`,
    THEORY_LINE,
    ADVISORY_LINE,
    campStanceLine(args.camp),
    "論じる対象は 2 つ: (1) 提案された破壊的な一手そのもの、(2) 実行したときに起きることとリスク。",
    "実在の人間がチャットで話すように、自然な口語で 2〜3 文だけ書く。ラベルや箇条書きは使わない。",
    "直前の相手陣営の発言があれば、それに正面から応える。既出の理由の言い換えは禁止。",
    "発言テキストのみを返す。",
  ].join("\n");
  const prompt = [
    `# 議題\n${args.theme}`,
    "",
    `# 破壊的な一手の提案\n${args.proposal}`,
    "",
    `# ここまでの発言\n${linesBlock(args.lines)}`,
    "",
    "あなたの発言:",
  ].join("\n");
  return { system, prompt };
}

/** 予測 prompt: 3 / 5 / 10 年後 + ビジネス分析 (利益率 / 確度 / ノウハウ強度)。 */
export function buildForecastPrompt(args: {
  theme: string;
  conclusion: string;
  proposal: string;
  lines: readonly ForesightLine[];
}): { system: string; prompt: string } {
  const system = [
    "あなたはイノベーション理論に通じた戦略アナリストで、議論の進行役。",
    "提案された破壊的な一手と、改革派・保守派の議論を踏まえ、その結果訪れる未来を予測する。",
    ADVISORY_LINE,
    "次の見出しで、日本語で書く (全体で 1500 字以内):",
    "【議論の決着】改革派と保守派で、どの論点が残り、どちらの前提がより妥当だったか",
    "【3年後】起きていること (足場の市場・普及段階)",
    "【5年後】起きていること (主流市場への侵食・既存企業の対応)",
    "【10年後】起きていること (新しい主流と、次に破壊される側)",
    "各年の予測には、そうなる確からしさ (高 / 中 / 低) と、外れる条件を一言添える。",
    "【ビジネス分析】議題がビジネススキーム (収益を得る事業・商品・サービス) の場合だけ、次の 3 項目を 高 / 中 / 低 と根拠で書く:",
    "- 利益率の高さ: 原価・価格決定力・規模の効き方から",
    "- 確度: 前提が成り立つ確からしさと、検証できるまでの距離",
    "- ノウハウの強度 (真似されにくさ): 競合が追随するのに要るもの (データ・組織能力・関係資産・特許など) と、追随できない理由",
    "ビジネススキームでない場合は【ビジネス分析】に「対象外 (理由)」とだけ書く。",
    "前置きや結びの挨拶は書かない。",
  ].join("\n");
  const prompt = [
    `# 議題\n${args.theme}`,
    "",
    conclusionBlock(args.conclusion),
    "",
    `# 破壊的な一手の提案\n${args.proposal}`,
    "",
    `# 改革派 / 保守派の議論\n${linesBlock(args.lines)}`,
    "",
    "未来予測:",
  ].join("\n");
  return { system, prompt };
}
