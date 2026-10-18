/**
 * 未来予測アドバイザリーの結果をディスカッションペーパー本文 (body_md) の末尾節にする (pure)。
 *
 * 既存の節は取り除いてから付け直す (冪等。やり直し時に重ならない)。
 * 節は結論とは別の参考 (アドバイザリー) なので、見出しにもそう書く。
 */

const HEAD_FORESIGHT = "# 未来予測アドバイザリー (参考)";
/**
 * 取り除く対象の見出し行 (現行 + 2026-10-18 初版の「# 未来予測」)。行頭から行末までの一致で見るので、
 * 本文の先頭にある見出しや、末尾に改行が無い見出しも外せる。
 */
const STRIP_HEAD_RE = /^# 未来予測(?: ?アドバイザリー \(参考\))?[ \t]*$/m;

export interface ForesightSection {
  proposal: string;
  forecast: string;
}

/** body_md から未来予測の節以降を取り除く。 */
export function stripForesight(bodyMd: string): string {
  const match = STRIP_HEAD_RE.exec(bodyMd);
  const base = match ? bodyMd.slice(0, match.index) : bodyMd;
  return base.replace(/\s+$/g, "");
}

/** body_md の末尾に未来予測の節 (提案 + 予測) を付ける。 */
export function appendForesightMarkdown(bodyMd: string, section: ForesightSection): string {
  const foresight = [
    HEAD_FORESIGHT,
    "結論とは別の参考です。結論は上の「# 結論」を正とします。",
    `## 破壊的な一手の提案\n${section.proposal.trim() || "(提案なし)"}`,
    `## 未解決の前提と 3 / 5 / 10 年時点のシナリオ\n${section.forecast.trim() || "(予測なし)"}`,
  ].join("\n\n");
  const base = stripForesight(bodyMd);
  return base ? `${base}\n\n${foresight}` : foresight;
}
