#pragma once
// 議論終了時に Discord へ送る「議論内容のまとめ」の本文 (テーマ・規模・立場別の発言数・採択された意見・結論)。
// Discord の 1 メッセージ (2000 字) に収まるよう kDigestMaxLength (UTF-16 単位) 以内に切る。
#include <cstddef>
#include <string>

#include "flow/transcript/snapshot.hpp"

namespace di::flow::transcript {

inline constexpr std::size_t kDigestMaxLength = 1900;
/** 採択された意見を何件まで載せるか。 */
inline constexpr std::size_t kDigestMaxWinners = 5;

struct DigestInput {
    std::string theme;
    std::string flowLabel;
    TranscriptSnapshot snapshot;
};

std::string render_transcript_digest(const DigestInput& input);

} // namespace di::flow::transcript
