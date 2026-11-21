import type { NativeScanReport } from "../verify-praesidium-native-coverage.js";

export default {
  post: (passed: boolean, report: NativeScanReport, units: string[]) => {
    return (passed && report.tool === "Praesidium" && units.length > 0
      && report.coverage.nativeUnits >= units.length
      && units.every(unit => report.checks.some(check => check.file === unit && check.rule === "native.clang-analysis")))
      || "Praesidium must pass and report native analysis for each required translation unit";
  },
};
