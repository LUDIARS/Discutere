#pragma once
// prefix-cache 整形 (ordering.ts)。不変な断片を前に、毎回変わる断片を後ろに並べてヒット率を上げる。
#include <cstddef>
#include <vector>

#include "lib/llm_gateway/types.hpp"

namespace di::lib::llm_gateway {

struct OrderedPrompt {
    /** 安定度順 (fixed → semi → volatile) に整列・結合したメッセージ列。 */
    std::vector<ChatMessage> messages;
    /** messages[0 .. cacheBreakpoint - 1] が「安定プレフィックス」。 */
    std::size_t cacheBreakpoint = 0;
};

/**
 * 断片を安定度順に並べ替え、同役割・同安定区分の隣接断片を 1 メッセージに結合する。
 * 空文字 (trim 後) の断片は捨てる。元の順序は同安定度内で保持する (安定ソート)。
 */
OrderedPrompt order_segments(const std::vector<Segment>& segments);

} // namespace di::lib::llm_gateway
