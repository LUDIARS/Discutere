#pragma once
// src/flow/paper-blocks.ts の移植: ディスカッションペーパー本文 (markdown) のブロック分解 / 再結合 (Notion 風編集の基盤)。
// ブロック id は出現順 (b0, b1, …)。1 回の編集ごとに md → split_blocks し直すので id は「現在の表示順」を指す。
// DB / LLM 非依存の純関数のみ。
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/json.hpp"

namespace di::flow {

/** "heading" | "paragraph" | "list" */
using BlockType = std::string;

struct PaperBlock {
    std::string id;
    BlockType type;
    /** ブロックの md 本文 (見出しは # 込み・箇条書きは複数行)。 */
    std::string text;
};

/** markdown を見出し / 段落 / 箇条書きブロックに分解する (空行は区切りのみ)。 */
std::vector<PaperBlock> split_blocks(std::string_view md);
/** ブロック列を markdown に戻す (ブロック間は空行 1 つ)。空ブロックは捨てる。 */
std::string join_blocks(const std::vector<std::string>& texts);
std::string join_blocks(const std::vector<PaperBlock>& blocks);
/** 指定ブロックを差し替えた md。newText が空 (trim 後) なら削除。blockId 不一致なら元の md のまま。 */
std::string replace_block(std::string_view md, std::string_view block_id, std::string_view new_text);
/** 指定ブロックの直後に段落ブロックを挿入した md (blockId 不一致なら末尾)。text が空なら元の md。 */
std::string insert_block_after(std::string_view md, std::string_view block_id, std::string_view text);
/** blockId のブロック本文 (無ければ nullopt)。 */
std::optional<std::string> get_block_text(std::string_view md, std::string_view block_id);

Json to_json(const PaperBlock& block);

} // namespace di::flow
