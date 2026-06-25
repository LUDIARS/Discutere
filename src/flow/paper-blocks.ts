/**
 * ディスカッションペーパー本文 (markdown) の **ブロック分解 / 再結合** (Notion 風編集の基盤)。
 *
 * Web の編集 UI は md を「ブロック」(見出し / 段落 / 箇条書き) 単位で並べ、
 * ブロックごとに LLM レビューやクロール補佐をかける。確定すると joinBlocks で md に戻す。
 *
 * ブロック id は出現順 (`b0`, `b1`, …)。1 回の編集ごとに md → splitBlocks し直すので
 * id は「現在の表示順」を指す安定キーとして使う (UI は blockId で差し替え対象を指定する)。
 *
 * DB / LLM 非依存の純関数のみ (単体テスト可)。
 */

export type BlockType = "heading" | "paragraph" | "list";

export interface PaperBlock {
  /** 表示順の安定キー (`b<index>`)。 */
  id: string;
  type: BlockType;
  /** ブロックの md 本文 (見出しは `#` マーカー込み・箇条書きは複数行)。 */
  text: string;
}

const HEADING_RE = /^#{1,6}\s+/;
const LIST_RE = /^(\s*[-*+]\s+|\s*\d+[.)]\s+)/;

/**
 * markdown を見出し / 段落 / 箇条書きブロックに分解する。
 *   - `# 見出し` 行 → heading ブロック (1 行 1 ブロック)。
 *   - 連続する箇条書き行 → 1 つの list ブロック。
 *   - 空行で区切られた連続行 → 1 つの paragraph ブロック。
 * 空行はブロックにしない (区切りのみ)。
 */
export function splitBlocks(md: string): PaperBlock[] {
  const lines = md.split(/\r?\n/);
  const blocks: Array<{ type: BlockType; lines: string[] }> = [];
  let buf: { type: BlockType; lines: string[] } | null = null;

  const flush = () => {
    if (buf && buf.lines.length > 0) blocks.push(buf);
    buf = null;
  };

  for (const line of lines) {
    if (line.trim() === "") {
      flush();
      continue;
    }
    if (HEADING_RE.test(line)) {
      flush();
      blocks.push({ type: "heading", lines: [line] });
      continue;
    }
    const type: BlockType = LIST_RE.test(line) ? "list" : "paragraph";
    if (!buf || buf.type !== type) {
      flush();
      buf = { type, lines: [] };
    }
    buf.lines.push(line);
  }
  flush();

  return blocks.map((b, i) => ({ id: `b${i}`, type: b.type, text: b.lines.join("\n") }));
}

/** ブロック列を markdown に戻す (ブロック間は空行 1 つ)。空ブロックは捨てる。 */
export function joinBlocks(blocks: Array<{ text: string }>): string {
  return blocks
    .map((b) => b.text.replace(/\s+$/g, ""))
    .filter((t) => t.trim() !== "")
    .join("\n\n");
}

/**
 * 指定ブロックの本文を差し替えて md を返す。
 * newText が空 (trim 後) ならそのブロックを削除する。blockId 不一致なら元の md のまま。
 */
export function replaceBlock(md: string, blockId: string, newText: string): string {
  const blocks = splitBlocks(md);
  const idx = blocks.findIndex((b) => b.id === blockId);
  if (idx < 0) return md;
  if (newText.trim() === "") {
    blocks.splice(idx, 1);
  } else {
    blocks[idx] = { ...blocks[idx], text: newText.replace(/\s+$/g, "") };
  }
  return joinBlocks(blocks);
}

/** 指定ブロックの直後に新しい段落ブロックを挿入して md を返す。 */
export function insertBlockAfter(md: string, blockId: string, text: string): string {
  if (text.trim() === "") return md;
  const blocks: Array<{ id: string; type: BlockType; text: string }> = splitBlocks(md);
  const idx = blocks.findIndex((b) => b.id === blockId);
  const node = { id: "new", type: "paragraph" as BlockType, text: text.replace(/\s+$/g, "") };
  if (idx < 0) blocks.push(node);
  else blocks.splice(idx + 1, 0, node);
  return joinBlocks(blocks);
}

/** blockId のブロック本文を取り出す (無ければ null)。 */
export function getBlockText(md: string, blockId: string): string | null {
  const block = splitBlocks(md).find((b) => b.id === blockId);
  return block ? block.text : null;
}
