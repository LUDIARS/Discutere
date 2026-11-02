#pragma once
// src/flow/sparring-commands.ts の移植: 壁打ちフローの終了コマンド検出 (sparring.md)。
//   「まとめて」→ そのラウンドを終了 / 「おわり」→ 議論を終了しまとめに入る。
// 分類器 (LLM) ではなく文字列一致。短い命令文 (正規化後 16 文字以内) に限定して誤検知を抑える。
#include <string>
#include <string_view>

namespace di::flow {

/** "summarize" | "end" | "none" */
using SparringCommand = std::string;

SparringCommand detect_sparring_command(std::string_view text);

} // namespace di::flow
