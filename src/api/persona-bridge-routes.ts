import { Hono } from "hono";

import { getConfig } from "../config.js";
import { createCore } from "../core/index.js";
import { resolveActiveKgPath } from "../core/kg-registry.js";
import {
  createCernerePublicKeyLoader,
  type PublicKeyLoader,
} from "../cernere-service-token/cernere-public-keys.js";
import { authorizeBridgeRequest, PERSONA_BRIDGE_SERVICE_SCOPE } from "../persona-bridge/bridge-auth.js";
import {
  PersonaBridgeAssertionConfigurationError,
  PersonaBridgeAssertionVerificationError,
  verifyPersonaBridgeAssertion,
  type VerifiedPersonaBridgeAssertion,
} from "../persona-bridge/authorization-assertion.js";
import {
  consumeDefaultPersonaBridgeAssertion,
  type AssertionReplayConsumer,
} from "../persona-bridge/assertion-replay-store.js";
import {
  decodeUtteranceCursor,
  nextUtteranceCursor,
  readHumanUtterances,
  type ExportableUtteranceRecord,
  type UtteranceCursor,
} from "../persona-bridge/utterance-export.js";

const MAX_LIMIT = 1_000;
export const PERSONA_BRIDGE_ASSERTION_HEADER = "x-discutere-persona-assertion";
/** 認証集約 P4: Cernere service token でこの endpoint を呼ぶのに要る scope (bridge-auth.ts が正本)。 */
export { PERSONA_BRIDGE_SERVICE_SCOPE };

type UtteranceReader = (input: {
  authorId: string;
  after: UtteranceCursor;
  limit: number;
}) => ExportableUtteranceRecord[];

function parseSince(value: string | undefined): number {
  if (!value) return 0;
  const parsed = /^\d+$/.test(value) ? Number(value) : Date.parse(value);
  if (!Number.isSafeInteger(parsed) || parsed < 0) {
    throw new Error("since must be an ISO date or epoch ms");
  }
  return parsed;
}

function parseLimit(value: string | undefined): number {
  const raw = value ?? "500";
  if (!/^\d+$/.test(raw)) throw new Error(`limit must be an integer in 1..${MAX_LIMIT}`);
  const parsed = Number(raw);
  if (!Number.isSafeInteger(parsed) || parsed < 1 || parsed > MAX_LIMIT) {
    throw new Error(`limit must be an integer in 1..${MAX_LIMIT}`);
  }
  return parsed;
}

type UtteranceQuery = Parameters<UtteranceReader>[0];

function parseQuery(
  authorId: string,
  since: string | undefined,
  cursor: string | undefined,
  limit: string | undefined
): { value: UtteranceQuery } | { error: string } {
  try {
    if (since && cursor) throw new Error("use either since or cursor, not both");
    const after = cursor
      ? decodeUtteranceCursor(cursor)
      : { postedAt: parseSince(since), id: "" };
    return { value: { authorId, after, limit: parseLimit(limit) } };
  } catch (error) {
    return { error: (error as Error).message };
  }
}

// @implements SPEC-PERSONA-BRIDGE-UTTERANCE-EXPORT
function defaultReader(input: UtteranceQuery): ExportableUtteranceRecord[] {
  const core = createCore(resolveActiveKgPath(getConfig()));
  try {
    return readHumanUtterances(core.client.raw, input);
  } finally {
    core.close();
  }
}

const defaultServiceTokenKeyLoader = createCernerePublicKeyLoader({
  baseUrl: () => process.env.CERNERE_BASE_URL ?? "",
});

// @implements SPEC-PERSONA-BRIDGE-UTTERANCE-EXPORT
export function createPersonaBridgeRoutes({
  readUtterances = defaultReader,
  token = () => process.env.DISCUTERE_PERSONA_BRIDGE_TOKEN ?? "",
  serviceAudience = () => process.env.CERNERE_PROJECT_STORAGE_SLUG ?? "",
  loadServiceTokenKeys = defaultServiceTokenKeyLoader,
  assertionPublicKey = () => process.env.DISCUTERE_PERSONA_BRIDGE_ASSERTION_PUBLIC_KEY ?? "",
  consumeAssertion = consumeDefaultPersonaBridgeAssertion,
  now = () => Date.now(),
}: {
  readUtterances?: UtteranceReader;
  token?: () => string;
  serviceAudience?: () => string;
  loadServiceTokenKeys?: PublicKeyLoader;
  assertionPublicKey?: () => string;
  consumeAssertion?: AssertionReplayConsumer;
  now?: () => number;
} = {}): Hono {
  const routes = new Hono();
  routes.get("/persona-bridge/utterances", async (context) => {
    context.header("cache-control", "private, no-store");
    // 認証集約 P4: Cernere service token と従来の固定トークンの両受理 (bridge-auth.ts)。
    // 本人の生発話を返す経路なので、固定トークンは 32 文字未満を未設定 (503) と扱い default-deny を保つ。
    const auth = await authorizeBridgeRequest(context.req.header("authorization"), {
      token,
      serviceAudience,
      loadServiceTokenKeys,
      now,
      label: "persona-bridge",
    });
    if (!auth.ok) return context.json({ ok: false, error: auth.error }, auth.status);
    const publicKey = assertionPublicKey();
    if (!publicKey.trim()) {
      return context.json({
        ok: false,
        error: "persona bridge assertion verification is not configured",
      }, 503);
    }
    const authorId = context.req.query("authorId") ?? "";
    if (!/^[0-9]{5,24}$/.test(authorId)) {
      return context.json({ ok: false, error: "authorId must be a Discord snowflake" }, 400);
    }
    // クエリ検証の失敗だけを 400 で返す。読み出しの失敗 (DB パスを含む例外等) は
    // 応答にも共有ログにも詳細を出さず、500 + 定型文にする。
    const query = parseQuery(
      authorId,
      context.req.query("since"),
      context.req.query("cursor"),
      context.req.query("limit")
    );
    if ("error" in query) return context.json({ ok: false, error: query.error }, 400);
    const nowMs = now();
    let assertion: VerifiedPersonaBridgeAssertion;
    try {
      assertion = verifyPersonaBridgeAssertion({
        compact: context.req.header(PERSONA_BRIDGE_ASSERTION_HEADER) ?? "",
        publicKey,
        authorId,
        nowMs,
      });
    } catch (error) {
      if (error instanceof PersonaBridgeAssertionConfigurationError) {
        console.warn(`[persona-bridge] assertion configuration failed: ${error.message}`);
        return context.json({
          ok: false,
          error: "persona bridge assertion verification is not configured",
        }, 503);
      }
      if (error instanceof PersonaBridgeAssertionVerificationError) {
        return context.json({ ok: false, error: "unauthorized" }, 401);
      }
      throw error;
    }
    try {
      if (!consumeAssertion(assertion, nowMs)) {
        return context.json({ ok: false, error: "unauthorized" }, 401);
      }
      // 1 件だけ lookahead し、実際に次の行がある場合に限って cursor を返す。
      // 応答上限は query の limit のままなので、lookahead 行の本文は外へ出さない。
      const records = readUtterances({ ...query.value, limit: query.value.limit + 1 });
      const hasMore = records.length > query.value.limit;
      const utterances = records.slice(0, query.value.limit);
      return context.json({
        ok: true,
        utterances: utterances.map(({ text, createdAt }) => ({ text, createdAt })),
        nextCursor: hasMore
          ? nextUtteranceCursor(utterances)
          : null,
      });
    } catch (error) {
      // DB path や接続先など、内部例外の本文を共有ログへ流さない。
      console.warn("[persona-bridge] utterance export failed");
      return context.json({ ok: false, error: "failed to read utterances" }, 500);
    }
  });
  return routes;
}

export const personaBridgeRoutes = createPersonaBridgeRoutes();
