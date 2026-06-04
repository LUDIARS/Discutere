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

export function buildConvergePrompt(input: {
  topic: string;
  recent: RecentUtterance[];
}): { system: string; user: string } {
  const system =
    "あなたは議論の進行役 (ファシリテーター) です。 十分に視点が出揃った議論を、" +
    " 中立の立場で要約し、 合意点・対立点・暫定結論を簡潔にまとめて締めます。 返答は JSON のみ。";
  const user =
    `# 議題\n${input.topic}\n\n` +
    `# これまでの議論\n${transcript(input.recent)}\n\n` +
    "# 指示\n議論を収束させる締めの要約を作ってください。 合意点 / 主要な対立軸 / 暫定結論を" +
    " 箇条書き中心で簡潔に。 返答は次の JSON のみ:\n" +
    `{ "summary": "<収束のまとめ (Discord 1 投稿に収まる長さ、 箇条書き可)>" }`;
  return { system, user };
}
