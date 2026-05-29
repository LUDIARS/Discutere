/**
 * JWT_SECRET production guard (PR-C / MISSING #4-5).
 *
 * env-cli.config.ts の default 値 (= "discutere-dev-secret-change-in-production")
 * をそのまま production で使うのを起動時に拒否する。
 *
 * NODE_ENV !== "production" では何もしない (= dev は default secret OK)。
 */

const DEFAULT_DEV_SECRET = "discutere-dev-secret-change-in-production";
const MIN_LENGTH = 32;

export interface JwtGuardOptions {
  /** test 用 — NODE_ENV を上書き */
  nodeEnv?: string;
  /** test 用 — JWT_SECRET を上書き */
  jwtSecret?: string;
}

export class JwtSecretError extends Error {
  constructor(message: string) {
    super(message);
    this.name = "JwtSecretError";
  }
}

export function assertProductionJwtSecret(options: JwtGuardOptions = {}): void {
  const env = options.nodeEnv ?? process.env.NODE_ENV ?? "";
  if (env !== "production") return;

  const secret = options.jwtSecret ?? process.env.JWT_SECRET ?? "";
  if (!secret) {
    throw new JwtSecretError("JWT_SECRET is not configured (required in production)");
  }
  if (secret === DEFAULT_DEV_SECRET) {
    throw new JwtSecretError(
      "JWT_SECRET is using the default dev value (= env-cli.config.ts default). Set a strong production secret via Infisical or env."
    );
  }
  if (secret.length < MIN_LENGTH) {
    throw new JwtSecretError(
      `JWT_SECRET is too short (${secret.length} chars, require >= ${MIN_LENGTH}).`
    );
  }
}
