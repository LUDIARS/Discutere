/** Shared primitives for strict bounded I/O validation, with no coercion or truncation. */
import { DiagnosticError, type DiagnosticErrorCode } from "./contracts.js";

export function fail(code: DiagnosticErrorCode, path: string, reason: string): never {
  throw new DiagnosticError(code, `${path}: ${reason}`);
}
export function object(value: unknown, path: string, keys: readonly string[], code: DiagnosticErrorCode): Record<string, unknown> {
  if (value === null || typeof value !== "object" || Array.isArray(value)) fail(code, path, "expected object");
  const result = value as Record<string, unknown>;
  if (Object.keys(result).some((key) => !keys.includes(key))) fail(code, path, "unexpected field");
  return result;
}
export function string(value: unknown, path: string, max: number, code: DiagnosticErrorCode): string {
  if (typeof value !== "string" || !value.trim() || value.length > max) fail(code, path, `expected nonempty string <= ${max} characters`);
  return value;
}
export function array(value: unknown, path: string, max: number, code: DiagnosticErrorCode, min = 0): unknown[] {
  if (!Array.isArray(value) || value.length < min || value.length > max) fail(code, path, `expected ${min}..${max} items`);
  return value;
}
export function finite(value: unknown, path: string, code: DiagnosticErrorCode): number {
  if (typeof value !== "number" || !Number.isFinite(value)) fail(code, path, "expected finite number");
  return value;
}
export function choice<T extends string>(value: unknown, choices: readonly T[], path: string, code: DiagnosticErrorCode): T {
  if (typeof value !== "string" || !choices.includes(value as T)) fail(code, path, `expected ${choices.join(" / ")}`);
  return value as T;
}
export function id(value: unknown, path: string, code: DiagnosticErrorCode): string {
  const result = string(value, path, 80, code);
  if (!/^[A-Za-z0-9][A-Za-z0-9_.:-]*$/.test(result)) fail(code, path, "invalid ID");
  return result;
}
export function unique(values: string[], path: string, code: DiagnosticErrorCode): void {
  if (new Set(values).size !== values.length) fail(code, path, "duplicate ID");
}
