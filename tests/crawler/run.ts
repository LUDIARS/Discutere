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
import "./external/importer.test.js";

console.log("crawler tests: all passed");
