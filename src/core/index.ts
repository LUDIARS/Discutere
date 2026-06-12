import { KuzuClient } from "./db/kuzu-client.js";
import type { KuzuGraphClient } from "./db/kuzu-graph-client.js";
import { EventLog } from "./events/event-log.js";
import { makeContext, createPersonRepo, createGameRepo, createMechanicRepo, createAestheticRepo, createAffectRepo, createPlayContextRepo, createSessionRepo, createUtteranceRepo, createReactionRepo, createDesignGapRepo, createHypothesisRepo } from "./repositories/base.js";
import { registerEmbedding } from "./vectors/embedding.js";
import { searchSimilar } from "./vectors/vector-search.js";

export interface CoreOptions {
  /** SQLite index DB パス。 */
  sqlitePath?: string;
  /** events.jsonl パス。 */
  eventsPath?: string;
  /** Kuzu グラフ DB クライアント（dual-write 期はオプショナル）。 */
  graph?: KuzuGraphClient;
}

export function createCore(sqlitePathOrOpts?: string | CoreOptions, eventsPath?: string) {
  const opts: CoreOptions = typeof sqlitePathOrOpts === "string"
    ? { sqlitePath: sqlitePathOrOpts, eventsPath }
    : (sqlitePathOrOpts ?? {});

  const client = new KuzuClient(opts.sqlitePath);
  const eventLog = new EventLog(client, opts.eventsPath, opts.graph);
  const ctx = makeContext(client, eventLog);

  return {
    client,
    eventLog,
    /** Kuzu グラフクライアント（設定済みの場合のみ。横断クエリに使う）。 */
    graph: opts.graph as KuzuGraphClient | undefined,
    repos: {
      person: createPersonRepo(ctx),
      game: createGameRepo(ctx),
      mechanic: createMechanicRepo(ctx),
      aesthetic: createAestheticRepo(ctx),
      affect: createAffectRepo(ctx),
      playContext: createPlayContextRepo(ctx),
      session: createSessionRepo(ctx),
      utterance: createUtteranceRepo(ctx),
      reaction: createReactionRepo(ctx),
      designGap: createDesignGapRepo(ctx),
      hypothesis: createHypothesisRepo(ctx),
    },
    vectors: {
      registerEmbedding: (input: { workspaceId: string; nodeType: string; nodeId: string; vector: number[] }) => registerEmbedding(client, input),
      searchSimilar: (input: { workspaceId: string; vector: number[]; k: number; nodeType?: string }) => searchSimilar(client, input),
    },
    close: () => {
      client.close();
      opts.graph?.close();
    },
  };
}

export * from "./events/event-types.js";
