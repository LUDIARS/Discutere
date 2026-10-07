/**
 * 論点ごとのプレイヤーの声 (外部の声) を、議論者のプロンプトに載せるブロックにする。
 *
 * dialectic エンジンは集めた外部の声をペーパー準備と事実照会にしか使っておらず、主張 (定立) と
 * 反論のプロンプトに一度も渡していなかった (2026-10-07 neco 指摘「集めた外部の声を参照していない」)。
 * 論点ごとに議題 + 論点で声を引き、出所付きで載せて、根拠のデータ (帰納の事例など) として使わせる。
 */

import type { ContextVoice } from "../discussion-paper.js";

/** 1 件の声の最大文字数 (プロンプトの肥大を防ぐ)。 */
const VOICE_CHARS = 160;

/** 声のブロック。声が無ければ空文字 (プロンプトに節を出さない)。 */
export function renderIssueVoices(voices: readonly ContextVoice[]): string {
  if (voices.length === 0) return "";
  const lines = voices.map((v, i) => {
    const body = v.content.replace(/\s+/g, " ").trim();
    const clipped = body.length > VOICE_CHARS ? `${body.slice(0, VOICE_CHARS)}…` : body;
    return `- [V${i + 1}] ${clipped}（出所: ${v.source}）`;
  });
  return [
    "# プレイヤーの声 (集めた外部の声。根拠のデータとして使う)",
    "主張・根拠・反論のデータには、まずここにある実際の声を [V番号] で引いて使う。声に無いことを事実として作らない。",
    ...lines,
  ].join("\n");
}
