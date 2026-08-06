import { parsePersonaDocument } from "./persona-import-document.js";

// @implements SPEC-PERSONA-BRIDGE-PERSONA-PULL
const MAX_PAGES = 100;
const LOOPBACK_HOSTS = new Set(["localhost", "127.0.0.1", "[::1]"]);

export interface PersonaPullResult {
  personas: unknown[];
  invalidJsonLines: number;
  pages: number;
}

export async function pullVoluptasPersonas({
  url,
  token,
  fetchImpl = fetch,
}: {
  url: string;
  token: string;
  fetchImpl?: typeof fetch;
}): Promise<PersonaPullResult> {
  const endpoint = new URL(url);
  if (
    endpoint.protocol !== "https:"
    && !(endpoint.protocol === "http:" && LOOPBACK_HOSTS.has(endpoint.hostname))
  ) {
    throw new Error("Voluptas export URL must use HTTPS (HTTP is allowed only on loopback)");
  }
  if (!token) throw new Error("DISCUTERE_VOLUPTAS_EXPORT_TOKEN is required");

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
        authorization: `Bearer ${token}`,
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
