/**
 * Magic-link `[[<type>:<id>]]` の parse / format / 検証。
 *
 * 仕様: spec/visualize/DESIGN.md「マジックリンク規約」 セクション。
 * 形式が崩れた link (typo / 未知 prefix / 空 id) は **失敗ではなく valid: false**
 * を返す (viewer 側で「dangling link」 として警告表示できるよう生かす方針)。
 */

export const NODE_TYPES = [
  "utt",
  "hyp",
  "gap",
  "mch",
  "aes",
  "aff",
  "ses",
  "game",
  "persona",
] as const;
export type NodeType = (typeof NODE_TYPES)[number];

export interface Wikilink {
  type: NodeType;
  id: string;
}

/** マジックリンク文字列を 1 件だけ parse。 ノイズ無し前提 (例: `[[hyp:abc]]`) */
export function parseWikilink(text: string): Wikilink | null {
  const trimmed = text.trim();
  const m = /^\[\[([a-z]+):([^\[\]\s]+)\]\]$/.exec(trimmed);
  if (!m) return null;
  const type = m[1];
  const id = m[2];
  if (!isNodeType(type)) return null;
  if (id.length === 0) return null;
  return { type, id };
}

/** Wikilink オブジェクト → 文字列 (`[[type:id]]`) */
export function formatWikilink(link: Wikilink): string {
  if (!isNodeType(link.type)) {
    throw new Error(`unknown node type: ${link.type}`);
  }
  if (!isValidId(link.id)) {
    throw new Error(`invalid id (must not contain whitespace or brackets): ${link.id}`);
  }
  return `[[${link.type}:${link.id}]]`;
}

/** 文字列内のすべての wikilink を抽出 (本文 / frontmatter 値の混合に対応) */
export function extractWikilinks(text: string): Wikilink[] {
  const out: Wikilink[] = [];
  const re = /\[\[([a-z]+):([^\[\]\s]+)\]\]/g;
  let match: RegExpExecArray | null;
  while ((match = re.exec(text)) !== null) {
    const type = match[1];
    const id = match[2];
    if (isNodeType(type) && isValidId(id)) {
      out.push({ type, id });
    }
  }
  return out;
}

/** node type ↔ Discatier table 名のマップ (resolver / importer から使う) */
export const TYPE_TO_TABLE: Record<NodeType, string> = {
  utt: "utterances",
  hyp: "hypotheses",
  gap: "design_gaps",
  mch: "mechanics",
  aes: "aesthetics",
  aff: "affects",
  ses: "sessions",
  game: "games",
  persona: "personas",
};

/** node type ↔ 出力 md ディレクトリ名のマップ */
export const TYPE_TO_DIR: Record<NodeType, string> = {
  utt: "utterance",
  hyp: "hypothesis",
  gap: "gap",
  mch: "mechanic",
  aes: "aesthetic",
  aff: "affect",
  ses: "session",
  game: "game",
  persona: "persona",
};

/** CLI 引数 / DESIGN.md の表記 (full word) と prefix のマップ */
export const FULL_TO_PREFIX: Record<string, NodeType> = {
  utterance: "utt",
  hypothesis: "hyp",
  gap: "gap",
  designgap: "gap",
  "design-gap": "gap",
  mechanic: "mch",
  aesthetic: "aes",
  affect: "aff",
  session: "ses",
  game: "game",
  persona: "persona",
};

/** "hypothesis" / "hyp" のどちらでも prefix に揃える */
export function normalizeType(input: string): NodeType | null {
  const lower = input.toLowerCase();
  if (isNodeType(lower)) return lower;
  const mapped = FULL_TO_PREFIX[lower];
  return mapped ?? null;
}

function isNodeType(value: string): value is NodeType {
  return (NODE_TYPES as readonly string[]).includes(value);
}

function isValidId(id: string): boolean {
  return /^[^\s\[\]]+$/.test(id);
}
