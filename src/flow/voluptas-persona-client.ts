import type { ServiceTokenProvider } from "../cernere-service-token/service-token-client.js";
import { parsePersonaDocument } from "./persona-import-document.js";

// @implements SPEC-PERSONA-BRIDGE-PERSONA-PULL
const MAX_PAGES = 100;
const LOOPBACK_HOSTS = new Set(["localhost", "127.0.0.1", "[::1]"]);
/** 認証集約 P4: Voluptas persona export を呼ぶ service token の宛先 (scope は persona-export:read)。 */
// Cernere の managed project key (Cernere migration 063 で volputas から改名)。
// 受け取る token の aud / sub に入る storage_slug は volputas のまま。
export const VOLUPTAS_TARGET_PROJECT_KEY = "voluptas";

/**
 * Cernere service token を優先し、 発行に失敗した時だけ固定トークンへ落とす (P4 移行期間)。
 * 理由コードだけを 1 行ログし、 token 本体は出さない。 P5 で固定トークン経路を撤去する。
 */
export async function resolveVoluptasBearer({
  serviceToken,
  fallbackToken,
  log = (line: string) => console.warn(line),
}: {
  serviceToken?: ServiceTokenProvider;
  fallbackToken: string;
  log?: (line: string) => void;
}): Promise<string> {
  if (serviceToken) {
    const issued = await serviceToken();
    if (issued.ok) return issued.token;
    if (fallbackToken) {
      log(`[persona-import] cernere service token unavailable (reason=${issued.reason}); using fixed token`);
      return fallbackToken;
    }
    throw new Error(
      `Cernere service token unavailable (reason=${issued.reason}) and DISCUTERE_VOLUPTAS_EXPORT_TOKEN is not set`
    );
  }
  if (!fallbackToken) throw new Error("DISCUTERE_VOLUPTAS_EXPORT_TOKEN is required");
  return fallbackToken;
}

export interface PersonaPullResult {
  personas: unknown[];
  invalidJsonLines: number;
  pages: number;
}

export async function pullVoluptasPersonas({
  url,
  token,
  serviceToken,
  fetchImpl = fetch,
  log,
}: {
  url: string;
  /** 従来の固定トークン。 P4 では service token 発行失敗時のフォールバック。 */
  token: string;
  serviceToken?: ServiceTokenProvider;
  fetchImpl?: typeof fetch;
  log?: (line: string) => void;
}): Promise<PersonaPullResult> {
  const endpoint = new URL(url);
  if (
    endpoint.protocol !== "https:"
    && !(endpoint.protocol === "http:" && LOOPBACK_HOSTS.has(endpoint.hostname))
  ) {
    throw new Error("Voluptas export URL must use HTTPS (HTTP is allowed only on loopback)");
  }
  const bearer = await resolveVoluptasBearer({ serviceToken, fallbackToken: token, log });

  const personas: unknown[] = [];
  let invalidJsonLines = 0;
  let pages = 0;
  let cursor: string | null = null;
  do {
    if (pages >= MAX_PAGES) throw new Error("Voluptas export exceeded the page limit");
    const pageUrl = new URL(endpoint);
    if (cursor) pageUrl.searchParams.set("cursor", cursor);
    const response = await fetchImpl(pageUrl, {
      redirect: "manual",
      headers: {
        accept: "application/x-ndjson",
        authorization: `Bearer ${bearer}`,
      },
    });
    // Authorization を別 endpoint へ転送しない。redirect は呼び出し元が明示的に URL を
    // 更新して再実行するまで拒否する。
    if (!response.ok) {
      throw new Error(`Voluptas export request failed with status ${response.status}`);
    }
    const parsed = parsePersonaDocument(await response.text());
    personas.push(...parsed.personas);
    invalidJsonLines += parsed.invalidJsonLines;
    pages += 1;
    cursor = response.headers.get("x-next-cursor");
  } while (cursor);

  return { personas, invalidJsonLines, pages };
}
