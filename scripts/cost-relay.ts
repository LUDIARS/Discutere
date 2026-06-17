/**
 * LLM コストを Anatomia のコスト削減UIへ一回 PUSH する CLI。
 *
 *   npm run cost-relay                          # 全セッションを config/env の URL へ送る
 *   npm run cost-relay -- --url http://host:4200
 *   npm run cost-relay -- --since <epochMs>     # since 以降に活動したセッションのみ
 *   npm run cost-relay -- --service discutere
 *
 * URL は引数 > env DISCUTERE_COST_RELAY_URL > config cost.relay.anatomiaUrl の順。
 * 定期送信はサーバ起動時 (cost.relay.enabled) に startCostRelay が回す。
 */

import { getConfig } from "../src/config.js";
import { getFlowDb } from "../src/flow/db/connection.js";
import { relayCostOnce } from "../src/flow/cost-relay.js";

interface Args {
  url?: string;
  service?: string;
  activeSince?: number;
}

function parseArgs(argv: string[]): Args {
  const a: Args = {};
  for (let i = 0; i < argv.length; i++) {
    const k = argv[i];
    if (k === "--url") a.url = argv[++i];
    else if (k === "--service") a.service = argv[++i];
    else if (k === "--since") a.activeSince = Number(argv[++i]);
  }
  return a;
}

const args = parseArgs(process.argv.slice(2));
const cfg = getConfig();
const url = args.url ?? cfg.cost.relay.anatomiaUrl;
const service = args.service ?? cfg.cost.relay.service;

if (!url) {
  console.error(
    "no Anatomia URL — pass --url, or set DISCUTERE_COST_RELAY_URL / cost.relay.anatomiaUrl",
  );
  process.exit(1);
}

const db = getFlowDb();
const result = await relayCostOnce(db, {
  baseUrl: url,
  service,
  activeSince: args.activeSince,
  ts: Date.now(),
});

if (result.ok) {
  console.log(`cost-relay: pushed ${result.pushed} session-rows to ${url} (recorded=${result.recorded ?? "?"})`);
} else {
  console.error(`cost-relay: push failed: ${result.error}`);
  process.exit(1);
}
