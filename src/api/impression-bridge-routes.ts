/**
 * Voluptas → Discutere の感想受け取り口 `POST /api/persona-bridge/impressions`
 * (spec/feature/flow/voluptas-impression-push.md)。
 *
 * Voluptas が自動で送る匿名の感想を、Cernere service token (aud=Discutere の storage_slug、
 * scope=persona-bridge:write) か移行期間の固定トークンで受ける (bridge-auth.ts)。
 * Discutere 自身は Cernere の credential を持たない (検証は Cernere の公開鍵だけ)。
 * 本文 `{ impressions: [...] }` → 匿名性・形の検査 → KG へ取込 (dedup) → 件数を返す。
 */

import { Hono } from "hono";

import { getConfig } from "../config.js";
import { createCore } from "../core/index.js";
import { resolveActiveKgPath } from "../core/kg-registry.js";
import { createOpenAiCompatEmbedder } from "../core/vectors/embedder.js";
import { embedVoicesByIds } from "../core/vectors/voice-index.js";
import { createCernerePublicKeyLoader, type PublicKeyLoader } from "../cernere-service-token/cernere-public-keys.js";
import type { ExternalUtterance } from "../crawler/sources/types.js";
import { defaultImport } from "../flow/learning-autocrawl.js";
import { authorizeBridgeRequest } from "../persona-bridge/bridge-auth.js";
import {
  ImpressionBatchError,
  ingestImpressions,
  parseImpressionBatch,
  type IngestDeps,
} from "../persona-bridge/impression-intake.js";

/** 本番の取込 (active KG を開いて importer + 埋め込み)。 */
async function defaultIngest(run: (deps: IngestDeps) => Promise<unknown>): Promise<void> {
  const cfg = getConfig();
  const core = createCore(resolveActiveKgPath(cfg));
  const embedder = cfg.embedding.enabled ? createOpenAiCompatEmbedder(cfg.embedding) : null;
  try {
    await run({
      importItems: (items: ExternalUtterance[]) => defaultImport(core, items, cfg.workspace),
      embed: embedder
        ? (ids) => embedVoicesByIds(core, embedder, ids, { workspaceId: cfg.workspace, batchSize: cfg.embedding.batchSize })
        : undefined,
      warn: (m) => console.warn(`[impression-bridge] ${m}`),
    });
  } finally {
    core.close();
  }
}

const defaultKeyLoader = createCernerePublicKeyLoader({ baseUrl: () => process.env.CERNERE_BASE_URL ?? "" });

export function createImpressionBridgeRoutes({
  token = () => process.env.DISCUTERE_PERSONA_BRIDGE_TOKEN ?? "",
  serviceAudience = () => process.env.CERNERE_PROJECT_STORAGE_SLUG ?? "",
  loadServiceTokenKeys = defaultKeyLoader,
  now = () => Date.now(),
  withIngest = defaultIngest,
}: {
  token?: () => string;
  serviceAudience?: () => string;
  loadServiceTokenKeys?: PublicKeyLoader;
  now?: () => number;
  /** 取込依存を用意して run を呼ぶ (テストで差し替える)。 */
  withIngest?: (run: (deps: IngestDeps) => Promise<unknown>) => Promise<void>;
} = {}): Hono {
  const routes = new Hono();
  routes.post("/persona-bridge/impressions", async (context) => {
    context.header("cache-control", "private, no-store");
    const auth = await authorizeBridgeRequest(context.req.header("authorization"), {
      token,
      serviceAudience,
      loadServiceTokenKeys,
      now,
      label: "impression-bridge",
    });
    if (!auth.ok) return context.json({ ok: false, error: auth.error }, auth.status);

    let parsed;
    try {
      parsed = parseImpressionBatch(await context.req.json());
    } catch (error) {
      const message = error instanceof ImpressionBatchError ? error.message : "body must be JSON";
      return context.json({ ok: false, error: message }, 400);
    }
    try {
      let result: Awaited<ReturnType<typeof ingestImpressions>> | null = null;
      await withIngest(async (deps) => {
        result = await ingestImpressions(parsed.impressions, deps);
      });
      const r = result ?? { received: 0, imported: 0, embedded: 0, games: {} };
      if (parsed.skipped.non_anonymous_field > 0) {
        console.warn(`[impression-bridge] 匿名でない項目を含む感想 ${parsed.skipped.non_anonymous_field} 件を拒否`);
      }
      return context.json({ ok: true, ...r, skipped: parsed.skipped });
    } catch {
      // KG パスなど内部例外の本文は応答にもログにも出さない。
      console.warn("[impression-bridge] impression ingest failed");
      return context.json({ ok: false, error: "failed to ingest impressions" }, 500);
    }
  });
  return routes;
}

export const impressionBridgeRoutes = createImpressionBridgeRoutes();
