import { performance } from "node:perf_hooks";

let startedAt = 0;

export default {
  pre: (_html: string): true => {
    startedAt = performance.now();
    return true;
  },
  post: (result: string, html: string): true | string => {
    if (performance.now() - startedAt >= 1000) return "HTML extraction exceeded one second";
    if (result !== result.trim() || /\s{2,}|[\t\r\n]/.test(result)) return "Whitespace is not normalized";
    for (const token of ["<script ", "<style ", "<a "]) {
      if (html.length >= 256 * 1024 && html === token.repeat(Math.ceil(256 * 1024 / token.length))) {
        return result === html.trim() ? true : "Unclosed markup must retain its text";
      }
    }
    return true;
  },
};
