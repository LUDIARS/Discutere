import type { NativeScanReport } from "./verify-praesidium-native-coverage.js";
import { contract } from './contracts/runtime.js'; /* augur-inject:import:57a3e239 */
import augurContract_0ff3ee07 from './contracts/playtest-runtime-clang.contract.js'; /* augur-inject:contract-predicate:a0da5882 */

const RUNTIME_TEST_UNIT = "native/tests/flow/contracts_runtime_tests.cpp";

/**
 * Requires a successful Clang check of the unit that instantiates the no-predicate wrapper.
 * @implements SPEC-DI-PLAYTEST-RUNTIME
 */
export function verifyPlaytestRuntimeClang(report: NativeScanReport): boolean {
  return report.tool === "Praesidium" && report.checks.some(check =>
    check.rule === "native.clang-analysis" && check.file === RUNTIME_TEST_UNIT && check.status === "passed"
  );
}
// @ts-expect-error augur-inject
verifyPlaytestRuntimeClang = contract(verifyPlaytestRuntimeClang, { ...augurContract_0ff3ee07, contractId: 'C-10', mode: 'observe', sample: 1, where: 'src/flow/verify-playtest-runtime-clang.ts:6', rule: 'contract-wrap', id: '0ff3ee07' }); /* augur-inject:contract-wrap:0ff3ee07 */
