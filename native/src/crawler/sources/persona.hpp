#pragma once
// 話者の同一性 (persona アンカー) と露出マスク — spec/feature/crawler/EXTERNAL-SOURCES.md §6
// (src/crawler/sources/persona.ts の移植)。
//
// 統治原則「情報精度 > プライバシー」を二層で実装する:
//  - 保管層: 公開・安定 ID をそのまま speaker_id にする (to_speaker_id)。persona 精度を保つ。
//  - 露出層: 議論ユーザに見せる時は不可逆なペルソナ表示名に置換する (masked_persona_label)。
//    個人特定情報 (公開 ID) は end user に渡さない。
#include <optional>
#include <string>
#include <string_view>

#include "base/json.hpp"

namespace di::crawler::sources {

/** 保管層: 公開・安定 ID を speaker_id アンカー化する (`ext:<source>:<authorId>`)。 */
std::string to_speaker_id(std::string_view source, std::string_view author_id);

/** 露出層: speaker_id を SHA-256 で一方向ハッシュし「論者#xxxxxx」(先頭 6 hex) へ変換する。 */
std::string masked_persona_label(std::string_view speaker_id);

/** 出所メタの最小形 (attribution-store の Attribution と互換)。 */
struct SourceAttribution {
    std::string source;
    std::string sourceUrl;
};

struct SourceBadge {
    std::string source;
    std::string url;
};

/** 出所バッジ (ソース種別 + 元 URL を end user に開示)。attribution 無しなら nullopt。 */
std::optional<SourceBadge> source_badge(const std::optional<SourceAttribution>& attribution);

struct MaskedSpeaker {
    std::string speaker;
    std::optional<std::string> source;     // null のとき nullopt
    std::optional<std::string> sourceUrl;  // null のとき nullopt
};
/** { speaker, source, sourceUrl } (null は JSON の null)。 */
Json to_json(const MaskedSpeaker& v);

/** 個人はペルソナ名へマスクし、出所は attribution があれば素通しで開示する。 */
MaskedSpeaker masked_speaker_with_source(std::string_view speaker_id, const std::optional<SourceAttribution>& attribution);

} // namespace di::crawler::sources
