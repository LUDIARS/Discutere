/** 議論タイプごとのプロジェクト前提。登録済みリポジトリの有無とは別の概念。 */
export const PROJECT_PREMISES = {
  discussion: {
    label: "企画/議論",
    description: "プロジェクトなし。目的やアイデアから企画を検討する",
    instruction: "既存プロジェクトがない企画段階として議論する。目的・アイデア・比較案から検討し、未定の仕様や現状を既存の事実として作らない。既存作品や外部の声は参考事例として区別する。プロジェクト名や既存実装の提出を開始条件にしない。",
  },
  improvement: {
    label: "改善/議論",
    description: "プロジェクトあり。現状と課題を踏まえて改善を検討する",
    instruction: "対象の既存プロジェクトがある前提で議論する。対象・現状・課題・制約を確認し、現状に対する変更案と効果を比較する。不明な現状は推測で埋めず、必要な情報を質問する。",
  },
} as const;

export function projectPremise(flow: string | undefined): string {
  if (flow !== "discussion" && flow !== "improvement") return "";
  const premise = PROJECT_PREMISES[flow];
  return `# 議論の前提\n${premise.label}: ${premise.instruction}`;
}

/** 企画はテーマだけで開始できる。既存の他フローの入力条件は維持する。 */
export function requiresProjectTitle(flow: string | undefined): boolean {
  return flow !== "discussion";
}
