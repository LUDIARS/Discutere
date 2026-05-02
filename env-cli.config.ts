import type { EnvCliConfig } from "../Cernere/packages/env-cli/src/types.js";

const config: EnvCliConfig = {
  name: "Discutere",

  /**
   * Docker Compose / アプリケーションが .env から読むインフラキー。
   * Infisical に同名キーがあればそちらを優先し、なければデフォルト値を使用。
   */
  infraKeys: {
    // ─── Ports ─────────────────────────────────────────────
    FRONTEND_PORT: "5174",
    BACKEND_PORT: "3100",

    // ─── Database (SQLite ローカル) ────────────────────────
    DATABASE_PATH: "data/discutere.db",

    // ─── Vite ──────────────────────────────────────────────
    VITE_ALLOWED_HOSTS: "",

    // ─── Application ───────────────────────────────────────
    FRONTEND_URL: "http://localhost:5174",
    CERNERE_URL: "http://localhost:8080",

    // ─── JWT (Discutere 独自の service_token 用) ───────────
    JWT_SECRET: "discutere-dev-secret-change-in-production",

    // ─── Cernere プロジェクト認証 (project_token / WS 接続用) ─
    CERNERE_PROJECT_CLIENT_ID: "",
    CERNERE_PROJECT_CLIENT_SECRET: "",
  },

  defaultSiteUrl: "https://app.infisical.com",
  defaultEnvironment: "dev",

  /**
   * production 環境で env-cli env / up を実行したとき、
   * Infisical に存在しない (= dev 用 placeholder のまま) と .env 生成を中止するキー。
   * dev fallback が本番に漏れると致命的になる項目を列挙する。
   */
  required: {
    production: ["JWT_SECRET", "CERNERE_PROJECT_CLIENT_SECRET"],
  },
};

export default config;
