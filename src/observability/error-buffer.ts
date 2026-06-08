/**
 * In-memory エラーリングバッファ (admin dashboard 可視化用)。
 *
 * console.error / console.warn を console-capture が拾い、 ここに積む。
 * 永続化はしない (プロセス内のみ / 上限 CAP 件で古いものから捨てる)。
 * SQLite やファイルに残さないのは「直近の異常を素早く見る」用途に絞るため。
 */

export type ErrorLevel = "error" | "warn";

export interface BufferedError {
  ts: number;
  level: ErrorLevel;
  source: string;
  message: string;
}

const CAP = 200;
const buffer: BufferedError[] = [];

/** 1 件記録する (古いものから CAP 超過分を捨てる)。 */
export function recordError(level: ErrorLevel, source: string, message: string): void {
  buffer.push({ ts: Date.now(), level, source, message });
  if (buffer.length > CAP) buffer.splice(0, buffer.length - CAP);
}

/** 直近のエラーを新しい順に返す (既定 200)。 */
export function recentErrors(limit = CAP): BufferedError[] {
  const n = Math.max(0, Math.min(limit, buffer.length));
  return buffer.slice(buffer.length - n).reverse();
}

/** バッファを空にする (dashboard の「クリア」)。 */
export function clearErrors(): void {
  buffer.length = 0;
}
