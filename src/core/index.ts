import { KuzuClient } from "./db/kuzu-client.js";
import { EventLog } from "./events/event-log.js";
import { makeContext, createPersonRepo, createGameRepo, createMechanicRepo, createAestheticRepo, createAffectRepo, createPlayContextRepo, createSessionRepo, createUtteranceRepo, createReactionRepo, createDesignGapRepo, createHypothesisRepo } from "./repositories/base.js";
import { registerEmbedding } from "./vectors/embedding.js";
import { searchSimilar } from "./vectors/vector-search.js";

export function createCore(path?: string, eventsPath?: string) {
  const client = new KuzuClient(path);
  const eventLog = new EventLog(client, eventsPath);
  const ctx = makeContext(client, eventLog);

  return {
    client,
    eventLog,
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
    close: () => client.close(),
  };
}

export * from "./events/event-types.js";
