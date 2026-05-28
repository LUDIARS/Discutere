export interface ParsedCommand { command: string; args: string[]; }

export function parseCommand(content: string): ParsedCommand | null {
  const text = content.trim();
  if (!text.startsWith("/")) return null;
  const body = text.slice(1).trim();
  if (!body) return null;
  const tokens = body.split(/\s+/);
  if (!tokens[0]) return null;
  return { command: tokens[0].toLowerCase(), args: tokens.slice(1) };
}
