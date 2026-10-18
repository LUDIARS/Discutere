/**
 * 未来予測の結果をディスカッションペーパー本文 (body_md) の末尾節にする (pure)。
 *
 * 既存の「# 未来予測」節は取り除いてから付け直す (冪等。やり直し時に重ならない)。
 */

const HEAD_FORESIGHT = "# 未来予測";

export interface ForesightSection {
  proposal: string;
  forecast: string;
}

/** body_md から「# 未来予測」節以降を取り除く。 */
export function stripForesight(bodyMd: string): string {
  const idx = bodyMd.indexOf(`\n${HEAD_FORESIGHT}\n`);
  const base = idx >= 0 ? bodyMd.slice(0, idx) : bodyMd;
  return base.replace(/\s+$/g, "");
}

/** body_md の末尾に「# 未来予測」節 (提案 + 予測) を付ける。 */
export function appendForesightMarkdown(bodyMd: string, section: ForesightSection): string {
  const foresight = [
    HEAD_FORESIGHT,
    `## 破壊的な一手の提案\n${section.proposal.trim() || "(提案なし)"}`,
    `## 3 / 5 / 10 年後の予測\n${section.forecast.trim() || "(予測なし)"}`,
  ].join("\n\n");
  const base = stripForesight(bodyMd);
  return base ? `${base}\n\n${foresight}` : foresight;
}
