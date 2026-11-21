import { contract } from './contracts/runtime.js'; /* augur-inject:import:1067105c */
import augurContract_2d01d361 from './contracts/playtest-runtime-allocation.contract.js'; /* augur-inject:contract-predicate:43aba687 */
interface CodeCheckReport {
  version: number;
  files: number;
  findings: { rule: string }[];
}

/** Checks an Anatomia report scoped to the playtest contract runtime header. */
export function verifyPlaytestRuntimeAllocation(report: CodeCheckReport): boolean {
  return report.version === 1 && report.files === 1
    && report.findings.every(finding => finding.rule !== "memory/c-allocation");
}
// @ts-expect-error augur-inject
verifyPlaytestRuntimeAllocation = contract(verifyPlaytestRuntimeAllocation, { ...augurContract_2d01d361, contractId: 'C-9', mode: 'observe', sample: 1, where: 'src/flow/verify-playtest-runtime-allocation.ts:8', rule: 'contract-wrap', id: '2d01d361' }); /* augur-inject:contract-wrap:2d01d361 */
