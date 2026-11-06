#pragma once
// src/flow/paper-reuse.ts の移植: ディスカッションペーパーの使い回し (spec/feature/flow/paper-reuse.md)。
// 既存ペーパーを議論番号で指定すると、メカニクス・ゲーム内容・保存した声を引き継ぐ。
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "flow/discussion_paper.hpp"

namespace di::flow {

/** 本文から使い回すペーパー番号を読む (「ペーパー#12」「paper#12」「ペーパー 12」、全角数字可)。無ければ nullopt。 */
std::optional<std::int64_t> parse_reuse_paper_no(const std::optional<std::string>& text);

struct ReusablePaper {
    std::int64_t discussionNo = 0;
    std::string sessionId;
    std::string theme;
    std::vector<MechanicSummary> mechanics;
    /** ゲーム内容 (本文の固定欄)。無ければ ""。 */
    std::string mechanicsContext;
    std::vector<ContextVoice> voices;
};

std::optional<ReusablePaper> load_reusable_paper(std::int64_t discussion_no);
/** 引き継ぐゲーム内容 (元ペーパーの内容 + 今回の資料。重複は除く)。 */
std::optional<std::string> merge_reused_context(const std::optional<ReusablePaper>& reused,
                                                const std::optional<std::string>& current);
std::string describe_reuse(std::int64_t no, const std::optional<ReusablePaper>& reused);

} // namespace di::flow
