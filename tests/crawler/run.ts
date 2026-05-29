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

console.log("crawler tests: all passed");
