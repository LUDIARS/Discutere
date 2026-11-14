#pragma once
// 議論 1 件を、議論画面 (web/flow/page.html の確定後の 2 ペイン表示) と同じ見た目の静的 HTML にする。
// JS は使わず、テンプレート web/flow/transcript.html に値を埋める。文字はすべて HTML エスケープする。
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "flow/transcript/snapshot.hpp"

namespace di::flow::transcript {

struct TranscriptHtmlInput {
    /** 議題 (見出しと <title>)。 */
    std::string theme;
    /** 議論タイプの表示名 (例「企画/議論」)。 */
    std::string flowLabel;
    /** 出力した時刻 (ミリ秒)。無ければ表示しない。 */
    std::optional<std::int64_t> generatedAtMs;
    TranscriptSnapshot snapshot;
};

/** 発言カードに使う本文の上限 (超えた分は省いて件数を書く)。添付の容量上限に収めるため。 */
inline constexpr std::size_t kTranscriptCardsMaxBytes = 6u << 20;

/** 発言の立場から CSS クラスを決める (pro / con / opinion / user / neutral)。 */
std::string utterance_stance_class(const std::string& stance, const std::string& role);
/** 立場バッジの文字 (賛成派 / 反対派 / 中立 / 人間)。 */
std::string utterance_stance_label(const std::string& stance, const std::string& role);

std::string render_transcript_html(const TranscriptHtmlInput& input, std::size_t cards_max_bytes = kTranscriptCardsMaxBytes);

} // namespace di::flow::transcript
