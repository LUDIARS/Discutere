/**
 * Phase 0 crawler tests runner.
 *
 *   npx tsx tests/crawler/run.ts
 *
 * 順に md-format → importer round-trip → GraphDB read を回す。
 */

import "./md-format.test.js";
import "./importer-roundtrip.test.js";
import "./graphdb-read.test.js";
import "./ai-runner.test.js";

// External discussion sources (Phase 1)
import "./external/persona.test.js";
import "./external/steam-map.test.js";
import "./external/youtube-map.test.js";
import "./external/youtube-fetch.test.js";
import "./external/youtube-livechat-map.test.js";
import "./external/niconico-map.test.js";
import "./external/website.test.js";
import "./external/reddit.test.js";
import "./external/importer.test.js";
import "./external/summary.test.js";
import "./external/attribution-store.test.js";
import "./external/steam-persona.test.js";

// データソース隔離 (退避 + 復元)
import "./quarantine.test.js";
import "../sentiment-core/golden.test.js";

console.log("crawler tests: all passed");
