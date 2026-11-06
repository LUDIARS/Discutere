#pragma once
// ローリング要約 (summary.ts)。直近 N ターンは生で残し、それより古い部分を要約 1 個に畳む。
// 要約の LLM 呼び出しは外から注入する (このライブラリは transport 非依存)。
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "lib/llm_gateway/types.hpp"

namespace di::lib::llm_gateway {

struct HistoryTurn {
    ChatRole role;
    std::string content;
    bool operator==(const HistoryTurn&) const = default;
};

struct RollingSummaryOptions {
    /** 履歴セクションのトークン予算。これを超えたら要約を起こす。 */
    double maxTokens = 0;
    /** 常に生で残す直近ターン数。 */
    long long keepRecent = 0;
    /** トークン推定関数 (既定は内蔵ヒューリスティック)。 */
    std::function<double(std::string_view)> estimateTokens;
};

struct RollingSummaryResult {
    /** 古いターンの要約。畳む必要が無ければ nullopt。 */
    std::optional<std::string> summary;
    /** 生で残した直近ターン。 */
    std::vector<HistoryTurn> recent;
    /** 実際に要約が起きたか。 */
    bool compacted = false;
};

/** 古いターン群を 1 つの要約テキストに畳む関数 (同期。呼んだスレッドで LLM を待つ)。 */
using Summarizer = std::function<std::string(const std::vector<HistoryTurn>&)>;

/** 履歴が予算内なら素通し、超過していれば直近 keepRecent を残して残りを summarize で 1 個に畳む。 */
RollingSummaryResult rolling_summary(const std::vector<HistoryTurn>& turns, const Summarizer& summarize,
                                     const RollingSummaryOptions& opts);

} // namespace di::lib::llm_gateway
