/**
 * Di の ② Transform — @ludiars/canalis の Transform<KgBatch> を実装する。
 *
 * RawRecord[] (Canalis YouTubeSource / RedditSource 産) を受けて、
 * 0+1 感情カスケードを走らせ、ExternalSentiment KgBatch[] を返す。
 *
 * ③ Save は @ludiars/canalis の KuzuSink (または Di の KuzuGraphClient への直書き) が担う。
 */

import type { Transform } from "@ludiars/canalis";
import type { KgBatch, RawRecord } from "@ludiars/canalis";
import { cascadeSentiment, type CascadeClients, type Signal } from "./sentiment/cascade.js";

export interface ExternalSentimentTransformOptions {
  /** 収集対象ゲームのスラグ (KG のゲームノード key と対応)。 */
  gameSlug: string;
  clients: CascadeClients;
  /** エラー時のコールバック (省略時は console.error)。 */
  onError?: (err: Error, record: RawRecord) => void;
}

function extractSignal(record: RawRecord): Signal | undefined {
  const m = record.meta;
  if (!m) return undefined;
  const upvotes = typeof m.upvotes === "number" ? m.upvotes : undefined;
  const votedUp = typeof m.votedUp === "boolean" ? m.votedUp : undefined;
  if (upvotes === undefined && votedUp === undefined) return undefined;
  return { upvotes, votedUp };
}

/**
 * Di の外部感情 Transform。
 * Canalis の `PipelineDeps.transforms` に `"di-external-sentiment"` として差し込む。
 */
export class ExternalSentimentTransform implements Transform<KgBatch> {
  readonly name = "di-external-sentiment";

  constructor(private readonly opts: ExternalSentimentTransformOptions) {}

  async run(records: RawRecord[]): Promise<KgBatch[]> {
    const nodes: KgBatch["nodes"] = [];
    const edges: KgBatch["edges"] = [];

    for (const rec of records) {
      const text = rec.text ?? "";
      const signal = extractSignal(rec);
      try {
        const result = await cascadeSentiment(text, signal, this.opts.clients);
        const id = `ext:${rec.source}:${rec.sourceId}`;
        nodes.push({
          label: "ExternalSentiment",
          key: id,
          props: {
            game_slug: this.opts.gameSlug,
            source: rec.source,
            native_id: rec.sourceId,
            polarity: result.polarity,
            score: result.score,
            confidence: result.confidence,
            tier: result.tier,
            fetched_at: rec.fetchedAt,
          },
        });
        edges.push({
          label: "SENTIMENT_FOR",
          from: { label: "ExternalSentiment", key: id },
          to: { label: "Game", key: this.opts.gameSlug },
          props: {},
        });
      } catch (err) {
        (this.opts.onError ?? console.error)(err as Error, rec);
      }
    }

    if (nodes.length === 0) return [];
    return [{ kind: "kg", nodes, edges }];
  }
}
