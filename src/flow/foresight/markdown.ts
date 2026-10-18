/**
 * 未来予測アドバイザリーの結果をディスカッションペーパー本文 (body_md) の末尾節にする (pure)。
 *
 * 既存の節は取り除いてから付け直す (冪等。やり直し時に重ならない)。
 * 節は結論とは別の参考 (アドバイザリー) なので、見出しにもそう書く。
 */

const HEAD_FORESIGHT = "# 未来予測アドバイザリー (参考)";
/** 取り除く対象の見出し (現行 + 2026-10-18 初版の見出し)。 */
const STRIP_HEADS = [HEAD_FORESIGHT, "# 未来予測"];

export interface ForesightSection {
  proposal: string;
  forecast: string;
}

/** body_md から未来予測の節以降を取り除く。 */
export function stripForesight(bodyMd: string): string {
  const indexes = STRIP_HEADS.map((h) => bodyMd.indexOf(`\n${h}\n`)).filter((i) => i >= 0);
  const base = indexes.length > 0 ? bodyMd.slice(0, Math.min(...indexes)) : bodyMd;
  return base.replace(/\s+$/g, "");
}

/** body_md の末尾に未来予測の節 (提案 + 予測) を付ける。 */
export function appendForesightMarkdown(bodyMd: string, section: ForesightSection): string {
  const foresight = [
    HEAD_FORESIGHT,
    "結論とは別の参考です。結論は上の「# 結論」を正とします。",
    `## 破壊的な一手の提案\n${section.proposal.trim() || "(提案なし)"}`,
    `## 3 / 5 / 10 年後の予測\n${section.forecast.trim() || "(予測なし)"}`,
  ].join("\n\n");
  const base = stripForesight(bodyMd);
  return base ? `${base}\n\n${foresight}` : foresight;
}
