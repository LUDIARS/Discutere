#pragma once
// src/flow/dialectic/issue-voices.ts の移植: 論点ごとのプレイヤーの声を議論者のプロンプトに載せるブロックにする。
#include <string>
#include <vector>

#include "flow/discussion_paper.hpp"

namespace di::flow::dialectic {

/** 声のブロック。声が無ければ空文字。 */
std::string render_issue_voices(const std::vector<ContextVoice>& voices);

} // namespace di::flow::dialectic
