/**
 * Facilitator の LLM プロンプト + JSON 抽出 — spec/facilitator/DESIGN.md。
 *
 * - expand: 議論が停滞した時、 新しい視点の persona を 1 体生成し開口一番を出す
 * - converge: persona が十分出揃った時、 議論を要約して結論を 1 本出す
 */

export interface RecentUtterance {
  speaker: string; // 表示用 (persona 名 or "参加者")
  content: string;
}

export interface GeneratedPersona {
  name: string; // 役割名 (例 "コスト懐疑")
  display_name: string; // 人名 (例 "費田 渋")
  description: string;
  traits: string[];
  speech_style: string;
  opening: string; // 開口一番 (新しい論点を 1〜2 文で)
}

/** LLM 応答テキストから最初の JSON オブジェクトを取り出す。 */
export function extractJsonObject(text: string): unknown {
  const start = text.indexOf("{");
  const end = text.lastIndexOf("}");
  if (start < 0 || end <= start) throw new Error("no JSON object in LLM output");
  return JSON.parse(text.slice(start, end + 1));
}

function transcript(recent: RecentUtterance[]): string {
  return recent.map((u) => `- ${u.speaker}: ${u.content}`).join("\n") || "(まだ発言なし)";
}

export function buildExpandPrompt(input: {
  topic: string;
  existingPersonaNames: string[];
  recent: RecentUtterance[];
}): { system: string; user: string } {
  const system =
    "あなたは議論の進行役 (ファシリテーター) です。 停滞した議論に、 まだ出ていない" +
    "新しい視点を持ち込む参加者を 1 人だけ作り、 その人の最初の一言を考えます。" +
    " 既存の参加者と立場が被らないようにしてください。 返答は JSON のみ。";
  const user =
    `# 議題\n${input.topic}\n\n` +
    `# 既に参加している立場\n${input.existingPersonaNames.join(" / ") || "(なし)"}\n\n` +
    `# 直近の流れ\n${transcript(input.recent)}\n\n` +
    "# 指示\n上記と被らない新しい視点の参加者を 1 人作り、 議論を広げる開口一番を出してください。\n" +
    "返答は次の JSON のみ:\n" +
    `{
  "name": "<役割名 (短い、 例: コスト懐疑)>",
  "display_name": "<日本人名 (例: 費田 渋)>",
  "description": "<その人の立場・関心 (1 文)>",
  "traits": ["<特徴>", "<特徴>"],
  "speech_style": "<口調の特徴 (1 文)>",
  "opening": "<開口一番。 新しい論点を 1〜2 文で。 鋭く具体的に>"
}`;
  return { system, user };
}

/**
 * 止揚(アウフヘーベン)判定: 直近の議論で、 対立する既存意見を統合して
 * 一段上の結論に至った「新しい止揚」が出たかを判定する。
 * 既出 (stockedSummaries) と実質同じなら found=false。
 */
export function buildAufhebungJudgePrompt(input: {
  topic: string;
  recent: RecentUtterance[];
  stockedSummaries: string[];
}): { system: string; user: string } {
  const system =
    "あなたは議論の進行役です。 議論の中に『アウフヘーベン(止揚)』、 すなわち" +
    " 対立する複数の意見を否定し合うのではなく、 両者を活かして一段上の新しい結論へ統合した見解が" +
    " 新たに現れたかを判定します。 単なる賛成・繰り返し・部分的な指摘は止揚ではありません。 返答は JSON のみ。";
  const stocked = input.stockedSummaries.length
    ? input.stockedSummaries.map((s, i) => `${i + 1}. ${s}`).join("\n")
    : "(まだ無し)";
  const user =
    `# 議題\n${input.topic}\n\n` +
    `# 既にストック済みの止揚 (これと実質同じなら found=false)\n${stocked}\n\n` +
    `# 直近の議論\n${transcript(input.recent)}\n\n` +
    "# 指示\n直近の議論に、 既出と異なる新しい止揚が現れたか判定してください。 返答は次の JSON のみ:\n" +
    `{ "found": <true/false>, "summary": "<止揚の要旨を 1〜2 文で。 found=false なら空文字>" }`;
  return { system, user };
}

export function buildConvergePrompt(input: {
  topic: string;
  recent: RecentUtterance[];
  aufhebungen?: string[];
  topOpinions?: string[];
}): { system: string; user: string } {
  const system =
    "あなたは議論の進行役 (ファシリテーター) です。 出揃った議論を中立の立場で要約し、" +
    " 到達した止揚 (統合された結論) と、 参加者の評価が高かった意見を軸に、 締めのまとめを作ります。 返答は JSON のみ。";
  const auf = input.aufhebungen?.length
    ? input.aufhebungen.map((s, i) => `${i + 1}. ${s}`).join("\n")
    : "(特になし)";
  const top = input.topOpinions?.length ? input.topOpinions.map((s) => `- ${s}`).join("\n") : "(特になし)";
  const user =
    `# 議題\n${input.topic}\n\n` +
    `# 到達した止揚 (最重要、 結論の軸にする)\n${auf}\n\n` +
    `# 評価の高かった意見 (リアクション順、 重視する)\n${top}\n\n` +
    `# これまでの議論\n${transcript(input.recent)}\n\n` +
    "# 指示\n上記の止揚と高評価意見を軸に、 議論の到達点を簡潔にまとめてください。 返答は次の JSON のみ:\n" +
    `{ "summary": "<収束のまとめ (Discord 1 投稿に収まる長さ、 箇条書き可)>" }`;
  return { system, user };
}
