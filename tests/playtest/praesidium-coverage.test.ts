import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { verifyNativeCoverage } from "../../src/flow/verify-praesidium-native-coverage.js";
import { verifyPlaytestRuntimeClang } from "../../src/flow/verify-playtest-runtime-clang.js";

const reportPath = process.argv[2];
assert.ok(reportPath, "Pass the path to the Praesidium JSON report.");

const units = [
  "native/src/playtest/proposal_client.cpp",
  "native/src/playtest/proposal_outbox.cpp",
  "native/src/playtest/review.cpp",
  "native/src/playtest/seed.cpp",
  "native/src/playtest/voice_store.cpp",
  "native/tests/playtest/pipeline_tests.cpp",
];
const report = JSON.parse(readFileSync(reportPath, "utf8").replace(/^\uFEFF/, ""));
assert.ok(verifyPlaytestRuntimeClang(report), "Clang must analyze the contract runtime test unit.");
assert.ok(verifyNativeCoverage(report, units), "Praesidium must pass and analyze all playtest units.");
