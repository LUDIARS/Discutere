import { contract } from './contracts/runtime.js'; /* augur-inject:import:96d52e3c */
import augurContract_f80134e2 from './contracts/praesidium-native-coverage.contract.js'; /* augur-inject:contract-predicate:253d61f2 */
export interface NativeScanReport {
  tool: string;
  status: string;
  findings: unknown[];
  checks: { file?: string; rule: string; status: string }[];
  coverage: { nativeUnits: number };
}

export function verifyNativeCoverage(report: NativeScanReport, units: string[]): boolean {
  const analyzed = new Set(report.checks.filter(check => check.rule === "native.clang-analysis").map(check => check.file));
  return report.tool === "Praesidium" && report.status === "passed" && report.findings.length === 0
    && report.checks.every(check => check.status === "passed")
    && units.length > 0 && units.every(unit => analyzed.has(unit));
}
// @ts-expect-error augur-inject
verifyNativeCoverage = contract(verifyNativeCoverage, { ...augurContract_f80134e2, contractId: 'C-8', mode: 'observe', sample: 1, where: 'src/flow/verify-praesidium-native-coverage.ts:9', rule: 'contract-wrap', id: 'f80134e2' }); /* augur-inject:contract-wrap:f80134e2 */
