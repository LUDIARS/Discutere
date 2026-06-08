/**
 * console.error / console.warn を監視して error-buffer に積む。
 *
 * 元の console 出力は維持したまま (= stdout/stderr へ通常通り出す)、 追加で
 * recordError に渡す。 監視自体が落ちないよう全体を try/catch で包む
 * (ログ処理の失敗でアプリ本体を巻き込まない)。
 */

import { recordError } from "./error-buffer.js";

let installed = false;

/** console.error/warn を 1 度だけ monkeypatch する (二重適用は no-op)。 */
export function installConsoleCapture(): void {
  if (installed) return;
  installed = true;

  const origError = console.error.bind(console);
  const origWarn = console.warn.bind(console);

  console.error = (...args: unknown[]): void => {
    origError(...args);
    try {
      recordError("error", inferSource(args), stringifyArgs(args));
    } catch {
      /* ロギングは本体を壊さない */
    }
  };

  console.warn = (...args: unknown[]): void => {
    origWarn(...args);
    try {
      recordError("warn", inferSource(args), stringifyArgs(args));
    } catch {
      /* ロギングは本体を壊さない */
    }
  };
}

/** console 引数を 1 本の文字列に結合する (Error は message を採用)。 */
function stringifyArgs(args: unknown[]): string {
  return args
    .map((a) => {
      if (a instanceof Error) return a.message;
      if (typeof a === "string") return a;
      try {
        return JSON.stringify(a);
      } catch {
        return String(a);
      }
    })
    .join(" ")
    .trim();
}

/**
 * 先頭引数の "  [foo] ..." / "foo:" のような prefix を source として推測する。
 * 推測できなければ "console"。
 */
function inferSource(args: unknown[]): string {
  const first = typeof args[0] === "string" ? args[0].trim() : "";
  const bracket = first.match(/^\[([^\]]+)\]/);
  if (bracket) return bracket[1];
  const prefix = first.match(/^([a-z0-9_-]+):/i);
  if (prefix) return prefix[1];
  return "console";
}
