export function inferRespondsTo(rawContent: string): string | null {
  const line = rawContent.split(/\r?\n/, 1)[0]?.trim() ?? "";
  const m = line.match(/^>\s*@uid:([A-Za-z0-9._:-]+)/);
  return m ? m[1] : null;
}
