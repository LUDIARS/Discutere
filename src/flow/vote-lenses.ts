/**
 * 投票の評価レンズ (respec 03, A4)。
 *
 * 同一モデル N 票の強相関 (実質 1 票) を崩すため、投票者ごとに評価観点 (レンズ) を
 * 割り当てる。ペルソナは注入しない (中立性は維持し、**観点だけ**分ける)。
 * config `flow.voteLenses[]` で上書き可 (既定は下記 3 種)。
 *
 * config.ts と vote.ts の双方から参照されるため、依存ゼロの単独ファイルに置く
 * (config → flow 本体 → config の循環 import を避ける)。
 */

export interface VoteLens {
  /** レンズ識別子 (logic / grounds / relevance 等)。 */
  key: string;
  /** 投票プロンプトに差し込む評価観点の指示文。 */
  instruction: string;
}

/** 既定レンズ 3 種 (論理 / 根拠 / テーマ適合)。 */
export const DEFAULT_VOTE_LENSES: readonly VoteLens[] = [
  { key: "logic", instruction: "論理の一貫性・飛躍のなさを最重視して選ぶ" },
  { key: "grounds", instruction: "根拠・具体例の裏付けを最重視して選ぶ" },
  { key: "relevance", instruction: "テーマへの適合・議論を前に進める度合いを最重視して選ぶ" },
];
