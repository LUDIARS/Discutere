/**
 * Anatomia 連携の型定義 (議論前のゲーム情報供給)。
 *
 * Anatomia ([[project_anatomia]]) はソースコード解析から対象リポの「ドメイン」を抽出する
 * 建築規約オラクル。本連携はその `domains list --json` 出力 (ドメインカード) を取得し、
 * Di のディスカッションペーパーの事前情報 (メカニクス) に精製する。
 *
 * 本モジュールは型のみ (依存なし)。取得は `client.ts`、精製は `refine.ts`。
 */

/** Anatomia `domains list --json` が返すドメインカード 1 件 (本連携が使うフィールドのみ)。 */
export interface AnatomiaDomainCard {
  /** ドメイン名 (例 "Trial & Judgment")。 */
  name: string;
  /** ドメインの説明 (実装観点。spec-draft 由来)。 */
  description: string;
  /** ドメインに紐づく実装メカニクスのスラグ (例 ["trial", "death-or-education"])。空配列あり。 */
  mechanics: string[];
  /** 根拠とした spec 節 (出所トレース用)。 */
  specRefs: string[];
  /** ドメイン境界の根拠 (コード構造観点。player 体験ではない)。 */
  rationale?: string;
}

/** Anatomia `domains list --json` のレスポンス全体。 */
export interface AnatomiaDomainsResponse {
  /** カードの保存ディレクトリ (`<repo>/spec/domains`)。 */
  dir: string;
  domains: AnatomiaDomainCard[];
}

/**
 * 解析対象の指定。登録済みプロジェクト名 (`project`) か、リポジトリ絶対パス (`repoPath`) の
 * どちらか一方。両方/どちらも無しは呼び出し側で弾く。
 */
export type AnatomiaSource = { project: string } | { repoPath: string };
