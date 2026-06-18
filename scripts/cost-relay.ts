/**
 * LLM コストをコスト表示面へ一回 PUSH する CLI。
 *
 *   npm run cost-relay                              # config/env の URL (両方可) へ送る
 *   npm run cost-relay -- --url http://host:4200    # Anatomia (/api/cost-feed)
 *   npm run cost-relay -- --concordia-url http://host:17330  # Concordia (/v1/cost-feed)
 *   npm run cost-relay -- --since <epochMs>         # since 以降に活動したセッションのみ
 *   npm run cost-relay -- --service discutere
 *
 * URL は引数 > env (DISCUTERE_COST_RELAY_URL / DISCUTERE_COST_RELAY_CONCORDIA_URL)
 * > config (cost.relay.anatomiaUrl / concordiaUrl) の順。両方指定すれば両方へ送る。
 * 定期送信はサーバ起動時 (cost.relay.enabled) に startCostRelay が回す。
 */

import { getConfig } from "../src/config.js";
import { getFlowDb } from "../src/flow/db/connection.js";
import { relayCostOnce, buildCostFeedTargets } from "../src/flow/cost-relay.js";

interface Args {
  url?: string;
  concordiaUrl?: string;
  service?: string;
  activeSince?: number;
}

function parseArgs(argv: string[]): Args {
  const a: Args = {};
  for (let i = 0; i < argv.length; i++) {
    const k = argv[i];
    if (k === "--url") a.url = argv[++i];
    else if (k === "--concordia-url") a.concordiaUrl = argv[++i];
    else if (k === "--service") a.service = argv[++i];
    else if (k === "--since") a.activeSince = Number(argv[++i]);
  }
  return a;
}

const args = parseArgs(process.argv.slice(2));
const cfg = getConfig();
const service = args.service ?? cfg.cost.relay.service;
const targets = buildCostFeedTargets({
  anatomiaUrl: args.url ?? cfg.cost.relay.anatomiaUrl,
  concordiaUrl: args.concordiaUrl ?? cfg.cost.relay.concordiaUrl,
});

if (targets.length === 0) {
  console.error(
    "no target URL — pass --url / --concordia-url, or set DISCUTERE_COST_RELAY_URL / " +
      "DISCUTERE_COST_RELAY_CONCORDIA_URL / cost.relay.{anatomiaUrl,concordiaUrl}",
  );
  process.exit(1);
}

const db = getFlowDb();
const result = await relayCostOnce(db, {
  targets,
  service,
  activeSince: args.activeSince,
  ts: Date.now(),
});

if (result.pushed === 0) {
  console.log("cost-relay: nothing to push (no active sessions)");
} else {
  for (const { target, push } of result.results) {
    const label = target.label ?? target.baseUrl;
    if (push.ok) {
      console.log(`cost-relay: pushed ${result.pushed} session-rows to ${label} (recorded=${push.recorded ?? "?"})`);
    } else {
      console.error(`cost-relay: push to ${label} failed: ${push.error}`);
    }
  }
}

if (!result.ok) process.exit(1);
