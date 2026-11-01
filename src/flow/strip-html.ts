import { decodeXmlEntities } from "./xml-entities.js";
import { contract } from './contracts/runtime.js'; /* augur-inject:import:88224029 */
import augurContract_208deb10 from './contracts/strip-html.contract.js'; /* augur-inject:contract-predicate:d540662e */

/** Remove one raw-text element kind without retrying searches over the same suffix. */
function removeBlocks(html: string, tag: "script" | "style"): string {
  // ASCII folding keeps offsets intact even for Unicode such as U+0130.
  const folded = html.replace(/[A-Z]/g, (letter) => letter.toLowerCase());
  const opening = `<${tag}`;
  const closing = `</${tag}>`;
  const parts: string[] = [];
  let copiedUntil = 0;
  let cursor = 0;
  while (cursor < html.length) {
    const start = folded.indexOf(opening, cursor);
    if (start === -1) break;
    cursor = start + opening.length;
    // Match the old regexp's ASCII word boundary, including punctuation attributes.
    if (/[a-z0-9_]/.test(folded.charAt(cursor))) continue;
    const openEnd = folded.indexOf(">", cursor);
    if (openEnd === -1) break;
    const closeStart = folded.indexOf(closing, openEnd + 1);
    // No later opening can have a closing tag either; keep the remaining input.
    if (closeStart === -1) break;
    parts.push(html.slice(copiedUntil, start), " ");
    cursor = closeStart + closing.length;
    copiedUntil = cursor;
  }
  parts.push(html.slice(copiedUntil));
  return parts.join("");
}

/** Preserve incomplete markup, as the previous <[^>]+> replacement did. */
function removeTags(html: string): string {
  const parts: string[] = [];
  let copiedUntil = 0;
  let cursor = 0;
  while (cursor < html.length) {
    const start = html.indexOf("<", cursor);
    if (start === -1) break;
    const end = html.indexOf(">", start + 1);
    if (end === -1) break;
    cursor = end + 1;
    if (end === start + 1) continue;
    parts.push(html.slice(copiedUntil, start), " ");
    copiedUntil = cursor;
  }
  parts.push(html.slice(copiedUntil));
  return parts.join("");
}

/** Extract specification text in O(html.length) time, retaining legacy pass order. */
export function stripHtml(html: string): string {
  return decodeXmlEntities(removeTags(removeBlocks(removeBlocks(html, "script"), "style")))
    .replace(/\s+/g, " ")
    .trim();
}
// @ts-expect-error augur-inject
stripHtml = contract(stripHtml, { ...augurContract_208deb10, contractId: 'C-2', mode: 'observe', sample: 1, where: 'src/flow/strip-html.ts:51', rule: 'contract-wrap', id: '208deb10' }); /* augur-inject:contract-wrap:208deb10 */
