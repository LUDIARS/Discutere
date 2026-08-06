import { timingSafeEqual } from "node:crypto";
import { Hono } from "hono";

import { getConfig } from "../config.js";
import { createCore } from "../core/index.js";
import { resolveActiveKgPath } from "../core/kg-registry.js";
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

// @implements SPEC-PERSONA-BRIDGE-UTTERANCE-EXPORT
/**
 * 本人の生発話を返す経路なので、推測可能な短い token を「設定済み」と見なさない。
 * 32 文字未満は未設定と同じ扱い (503) にして default-deny を保つ。
 */
const MIN_TOKEN_LENGTH = 32;
const MAX_LIMIT = 1_000;
export const PERSONA_BRIDGE_ASSERTION_HEADER = "x-discutere-persona-assertion";

type UtteranceReader = (input: {
  authorId: string;
  after: UtteranceCursor;
  limit: number;
}) => ExportableUtteranceRecord[];

function secureMatch(actual: string, expected: string): boolean {
  const left = Buffer.from(actual);
  const right = Buffer.from(expected);
  return left.length === right.length && timingSafeEqual(left, right);
}

function bearerToken(header: string | undefined): string {
  const match = /^Bearer ([^\s]+)$/.exec(header ?? "");
  return match?.[1] ?? "";
}

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

// @implements SPEC-PERSONA-BRIDGE-UTTERANCE-EXPORT
export function createPersonaBridgeRoutes({
  readUtterances = defaultReader,
  token = () => process.env.DISCUTERE_PERSONA_BRIDGE_TOKEN ?? "",
  assertionPublicKey = () => process.env.DISCUTERE_PERSONA_BRIDGE_ASSERTION_PUBLIC_KEY ?? "",
  consumeAssertion = consumeDefaultPersonaBridgeAssertion,
  now = () => Date.now(),
}: {
  readUtterances?: UtteranceReader;
  token?: () => string;
  assertionPublicKey?: () => string;
  consumeAssertion?: AssertionReplayConsumer;
  now?: () => number;
} = {}): Hono {
  const routes = new Hono();
  routes.get("/persona-bridge/utterances", (context) => {
    context.header("cache-control", "private, no-store");
    const expected = token();
    if (expected.length < MIN_TOKEN_LENGTH) {
      return context.json({
        ok: false,
        error: `persona bridge is not configured (DISCUTERE_PERSONA_BRIDGE_TOKEN must be at least ${MIN_TOKEN_LENGTH} characters)`,
      }, 503);
    }
    if (!secureMatch(bearerToken(context.req.header("authorization")), expected)) {
      return context.json({ ok: false, error: "unauthorized" }, 401);
    }
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
