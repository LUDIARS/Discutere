/**
 * 未来予測フェーズ (spec/feature/flow/foresight.md) の prompt 組み立て (pure)。
 *
 * イノベーション理論 (Christensen の破壊的イノベーション / プロダクトライフサイクル /
 * Rogers の普及理論 / Moore のキャズム) を「適用条件つきの道具」として渡し、
 *   1. 提案: 前提事実 → 理論の当てはめ (当てはまらないも可) → 破壊可能な一手 + 起きること + リスク
 *   2. 議論: 改革派 (賛同) と 保守派 (否定) が 成立条件 → 反証 → 確かめ方 の順に論じる
 *   3. 予測: 未解決の前提 + 3 / 5 / 10 年時点の条件付きシナリオ + ビジネス分析 (算定不能を許す)
 * の 3 段の prompt を返す。LLM 呼び出しは phase.ts が行う。
 *
 * 2026-10-18 の Fable / Astra 評価で、理論名の列挙と誘導的な見出し (「何を過剰供給しているか」
 * 「3年後に足場→5年後に侵食→10年後に新しい主流」) が理論の誤用と成功物語を招くと指摘されたため、
 * 見出しは判定を先に求め、成功の筋書きを前提にしない形にしている。
 */

export type ForesightCamp = "reform" | "conservative";

/** 議論の材料 (本議論の結論 + 未来予測フェーズ内の発言)。 */
export interface ForesightLine {
  personaName: string;
  camp: ForesightCamp;
  text: string;
}

/** 未来予測に渡す本議論の文脈 (ペーパー本文の要約材料 + 集めた外部の声)。 */
export interface ForesightContext {
  /** ディスカッションペーパー本文 (議題 / 議論内容 / 論点 / 議論の経過 = 止揚 / 結論)。長すぎる分は切る。 */
  paperMd: string;
  /** 本議論で集めた外部の声 (出所付き)。議題との関連は未確認の参考。 */
  voices: ReadonlyArray<{ content: string; source: string }>;
}

/** 陣営ターンの段階 (往復ごとに進める)。 */
export type CampStage = "conditions" | "refutation" | "verification";

/** 未来予測は結論を変えない参考 (アドバイザリー) であることを全 prompt に入れる。 */
const ADVISORY_LINE =
  "これは本議論の結論とは別の参考 (アドバイザリー)。本議論の結論を覆したり書き換えたりしない。";

/** 事実と推測を分ける指示 (根拠の取得経路が無いので、知らないことは不明と書かせる)。 */
const EVIDENCE_LINE =
  "事実は【前提事実】と渡された資料にあるものだけを使う。市場規模・成長率・競合の機能・制度は、資料に無ければ「未確認」と明記し、断定しない。数値を作らない。";

/** 理論の適用条件 (理論名だけでなく、当てはめてよい条件と当てはまらない場合を渡す)。 */
const THEORY_CARDS = [
  "道具にする理論と、当てはめてよい条件:",
  "- 破壊的イノベーション (Christensen): 「ローエンド型」は既存の主流顧客が性能を過剰に供給されていて、より安く簡素な代替で足りる層がいるときだけ。「新市場型」は既存製品を使っていない非消費者 (お金・技能・機会が無く使えない人) を特定できるときだけ。どちらも既存企業が追わない収益構造 (利益率が低い / 市場が小さい) が要る。単に安い・簡素・新しいだけでは破壊ではない。性能を上の顧客へ伸ばすのは持続的イノベーション。",
  "- プロダクトライフサイクル (導入 / 成長 / 成熟 / 衰退): 段階は「どの市場単位か」(製品カテゴリ / 個別製品 / 企業) を決めてから判定する。売上や普及の根拠が無ければ段階は「未確認」とする。",
  "- 普及理論 (Rogers): イノベーター / アーリーアダプター / アーリーマジョリティ / レイトマジョリティ / ラガード は採用時期の区分で、無料 / 有料の区分ではない。",
  "- キャズム (Moore): Rogers とは別の理論。アーリーアダプターとアーリーマジョリティの間の断絶で、越えるには参照できる成功事例とホールプロダクト (周辺の支援込みで使える状態) が要る。",
  "当てはまる型が無いときは「当てはまらない」と書いてよい。理論に合わせて事実を曲げない。",
].join("\n");

const CAMP_LABEL: Record<ForesightCamp, string> = {
  reform: "改革派",
  conservative: "保守派",
};

/** ペーパー本文を prompt に載せる上限 (字)。 */
export const CONTEXT_PAPER_LIMIT = 6000;
/** prompt に載せる外部の声の件数。 */
export const CONTEXT_VOICE_LIMIT = 8;

function conclusionBlock(conclusion: string): string {
  const text = conclusion.trim();
  return text ? `# ここまでの議論の結論\n${text}` : "# ここまでの議論の結論\n(結論なし)";
}

function linesBlock(lines: readonly ForesightLine[]): string {
  if (lines.length === 0) return "(まだ発言なし)";
  return lines.map((l) => `${l.personaName} [${CAMP_LABEL[l.camp]}]: ${l.text}`).join("\n");
}

/** 本議論の文脈ブロック (ペーパー本文 + 外部の声)。無ければ空文字。 */
export function contextBlock(context: ForesightContext | undefined): string {
  if (!context) return "";
  const parts: string[] = [];
  const paper = context.paperMd.trim();
  if (paper) {
    const cut = paper.length > CONTEXT_PAPER_LIMIT ? `${paper.slice(0, CONTEXT_PAPER_LIMIT)}\n…(以下略)` : paper;
    parts.push(`# 本議論の資料 (ディスカッションペーパー)\n${cut}`);
  }
  const voices = context.voices.slice(0, CONTEXT_VOICE_LIMIT);
  if (voices.length > 0) {
    parts.push(
      [
        "# 集めた外部の声 (参考。議題との関連は未確認。関係の無いものは使わない)",
        ...voices.map((v) => `- 「${v.content}」(出所: ${v.source})`),
      ].join("\n")
    );
  }
  return parts.join("\n\n");
}

/** 提案 prompt: 前提事実 → 理論の当てはめ → 破壊可能な一手 + 起きること + リスク。 */
export function buildProposalPrompt(
  theme: string,
  conclusion: string,
  context?: ForesightContext
): { system: string; prompt: string } {
  const system = [
    "あなたはイノベーション理論に通じた戦略アナリスト。",
    THEORY_CARDS,
    EVIDENCE_LINE,
    ADVISORY_LINE,
    "議題の対象について、現行のプロダクトライフサイクルを破壊可能な一手を検討する。",
    "次の見出しで、日本語で簡潔に書く (全体で 1200 字以内):",
    "【前提事実】判断に使う事実を、資料で確認できるもの (確認済み) と、推測 (仮説) と、分からないこと (不明) に分けて箇条書き",
    "【理論の当てはめ】対象の市場単位を決め、ライフサイクルの段階 (未確認も可) と、破壊の型 (ローエンド型 / 新市場型 / 持続的 / 当てはまらない) を、上の条件のどれが満たされ・どれが未確認かで判定する",
    "【破壊可能な一手】何を、誰に向けて、どの足場から始めるか (具体的な一手 1 つ)。型が当てはまらないなら、その前提で最も有力な一手と、破壊ではない理由",
    "【実行すると何が起きるか】顧客・既存企業・市場構造の順に、起きうる変化 (確定した未来として書かない)",
    "【リスク】失敗の筋書き、既存企業の反撃、実行上の制約",
    "前置きや結びの挨拶は書かない。",
  ].join("\n");
  const ctx = contextBlock(context);
  const prompt = [`# 議題\n${theme}`, "", conclusionBlock(conclusion), ...(ctx ? ["", ctx] : []), "", "提案:"].join(
    "\n"
  );
  return { system, prompt };
}

/** 陣営の立場行。本議論での賛否とは独立に、提案された一手について立つ。 */
export function campStanceLine(camp: ForesightCamp): string {
  return camp === "reform"
    ? "あなたは【改革派 (賛同)】。本議論での自分の賛否とは独立に、提案された破壊的な一手を支持する側に立つ。なぜ現行のライフサイクルを壊せるのか、実行で何が良く変わるのかを具体的に擁護する。リスクを指摘されたら、回避策か「それでも取るべき理由」で応える。"
    : "あなたは【保守派 (否定)】。本議論での自分の賛否とは独立に、提案された破壊的な一手に反対する側に立つ。既存顧客・既存の価値網・実行コスト・失敗時の損害から現行路線を守る理由を示す。反対するだけでなく、提案が失敗する具体的な筋書きか、成立に必要な未達の条件を出す。";
}

/** 往復の段階ごとの論点。 */
export function campStageLine(stage: CampStage): string {
  switch (stage) {
    case "conditions":
      return "この往復の論点: 一手が成り立つための条件。改革派は成立条件を挙げ、保守派はそのうち未達・未確認のものを突く。";
    case "refutation":
      return "この往復の論点: 反証。相手の主張の前提のうち、崩れたら主張が成り立たなくなるものを 1 つ挙げ、崩れる筋書きを示す。";
    case "verification":
      return "この往復の論点: 確かめ方。対立している前提を、誰が・何を・どの期間で観測すれば白黒つくかを 1 つ提案する。";
  }
}

/** 本議論での自分の発言 (人物の一貫性のため)。無ければ空文字。 */
function ownStatementsBlock(statements: readonly string[] | undefined): string {
  const list = (statements ?? []).map((s) => s.trim()).filter(Boolean);
  if (list.length === 0) return "";
  return ["# 本議論でのあなたの発言 (視点と口調の参考。立場はこれに縛られない)", ...list.map((s) => `- ${s}`)].join("\n");
}

/** 議論ターン prompt: 改革派 / 保守派として、段階ごとの論点で提案と帰結を論じる。 */
export function buildCampTurnPrompt(args: {
  theme: string;
  proposal: string;
  personaName: string;
  camp: ForesightCamp;
  lines: readonly ForesightLine[];
  stage?: CampStage;
  ownStatements?: readonly string[];
}): { system: string; prompt: string } {
  const system = [
    `あなたは議論の参加者「${args.personaName}」。`,
    THEORY_CARDS,
    EVIDENCE_LINE,
    ADVISORY_LINE,
    campStanceLine(args.camp),
    campStageLine(args.stage ?? "conditions"),
    "実在の人間がチャットで話すように、自然な口語で 2〜3 文だけ書く。ラベルや箇条書きは使わない。",
    "直前の相手陣営の発言があれば、それに正面から応える。既出の理由の言い換えは禁止。新しい根拠か条件を 1 つ以上出す。",
    "発言テキストのみを返す。",
  ].join("\n");
  const own = ownStatementsBlock(args.ownStatements);
  const prompt = [
    `# 議題\n${args.theme}`,
    "",
    `# 破壊的な一手の提案\n${args.proposal}`,
    ...(own ? ["", own] : []),
    "",
    `# ここまでの発言\n${linesBlock(args.lines)}`,
    "",
    "あなたの発言:",
  ].join("\n");
  return { system, prompt };
}

/** 確からしさの帯域の定義 (予測とビジネス分析の「確度」で共通に使う)。 */
const CONFIDENCE_BANDS =
  "確からしさは次の定義で書く (較正された確率ではない): 高 = 主要な前提が【前提事実】の確認済み事実で支えられ、反証の兆しが無い / 中 = 主要な前提の一部が仮説のまま / 低 = 主要な前提の多くが仮説か、議論で崩れかけた / 判定不能 = 判断材料が無い。";

/** 予測 prompt: 未解決の前提 + 3 / 5 / 10 年時点の条件付きシナリオ + ビジネス分析。 */
export function buildForecastPrompt(args: {
  theme: string;
  conclusion: string;
  proposal: string;
  lines: readonly ForesightLine[];
  context?: ForesightContext;
}): { system: string; prompt: string } {
  const system = [
    "あなたはイノベーション理論に通じた戦略アナリストで、議論の記録係。改革派・保守派のどちらの味方でもない。",
    "提案された破壊的な一手と、改革派・保守派の議論を踏まえ、その結果訪れうる未来を条件付きのシナリオで示す。",
    EVIDENCE_LINE,
    ADVISORY_LINE,
    CONFIDENCE_BANDS,
    "次の見出しで、日本語で書く (全体で 2000 字以内):",
    "【未解決の前提】勝ち負けは判定しない。議論で決着しなかった前提を箇条書きにし、それぞれに 改革派の見方 / 保守派の見方 / 白黒をつける観測 (何を・いつまでに) を書く",
    "【3年後】【5年後】【10年後】それぞれの時点について、次の 3 つのシナリオを書く。成功の筋書きを前提にしない:",
    "- 成立: 一手が狙いどおり定着する場合に起きていること",
    "- 限定的定着: 一部の層でだけ残る場合に起きていること",
    "- 撤退・吸収: 失敗するか既存企業に吸収される場合に起きていること",
    "各時点の最後に、どのシナリオに進んでいるかを見分ける先行指標 (何を観測し、どの値・どの状態ならどれか) と、そのシナリオの確からしさを書く",
    "【ビジネス分析】議題がビジネススキーム (収益を得る事業・商品・サービス) の場合だけ、次の 3 項目を書く。価格・原価・獲得費・運用費などの材料が無い項目は「算定不能」とし、算定に要るデータを書く:",
    "- 利益率の高さ: 高 / 中 / 低 / 算定不能 と根拠 (原価・価格決定力・獲得費・運用費・規模の効き方)",
    "- 確度: 高 / 中 / 低 / 判定不能 (上の定義) と、検証できるまでの距離",
    "- ノウハウの強度 (真似されにくさ): 高 / 中 / 低 / 算定不能。いま保有しているものと、これから形成できる見込みのもの (仮説) を分け、競合が別の手段で同じ成果を出せるかも書く",
    "ビジネススキームでない場合は【ビジネス分析】に「対象外 (理由)」とだけ書く。",
    "前置きや結びの挨拶は書かない。",
  ].join("\n");
  const ctx = contextBlock(args.context);
  const prompt = [
    `# 議題\n${args.theme}`,
    "",
    conclusionBlock(args.conclusion),
    ...(ctx ? ["", ctx] : []),
    "",
    `# 破壊的な一手の提案\n${args.proposal}`,
    "",
    `# 改革派 / 保守派の議論\n${linesBlock(args.lines)}`,
    "",
    "未来予測:",
  ].join("\n");
  return { system, prompt };
}
