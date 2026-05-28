import type { EnvCliConfig } from "../Cernere/packages/env-cli/src/types.js";

const config: EnvCliConfig = {
  name: "Discutere",

  /**
   * Docker Compose / アプリケーションが .env から読むインフラキー。
   * Infisical に同名キーがあればそちらを優先し、なければデフォルト値を使用。
   *
   * 2026-05-28: Cernere / Frontend 撤去 (Di-14)。 Cernere/JWT/FRONTEND 関連 ENV を削除。
   * Discord Bot 認証用 ENV は Di-1 で追加予定。
   */
  infraKeys: {
    // ─── Ports ─────────────────────────────────────────────
    BACKEND_PORT: "3100",

    // ─── Database (SQLite ローカル) ────────────────────────
    DATABASE_PATH: "data/discutere.db",
  },

  defaultSiteUrl: "https://app.infisical.com",
  defaultEnvironment: "dev",

  /**
   * production 環境で env-cli env / up を実行したとき、
   * Infisical に存在しない (= dev 用 placeholder のまま) と .env 生成を中止するキー。
   */
  required: {
    production: [],
  },
};

export default config;
