#pragma once
// 結論の公開設定 (glab / public)。移植元 src/integrations/conclusion-publication.ts。
// 境界を越えるのは id / title / conclusion / updatedAt / scope だけ (発話・作者・ペーパー補足は出さない)。
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "base/sqlite.hpp"

namespace di::integrations {

/** "private" | "public" | "glab" */
using PublicationScope = std::string;

struct PublishedConclusion {
    std::string id;
    std::string title;
    std::string conclusion;
    Json updatedAt;  // DB の値をそのまま (JS の number)
    std::string scope;  // "public" | "glab"
};

struct PublicationState {
    bool publishable = false;
    PublicationScope scope;
    std::string revision;
    std::string title;
    std::string conclusion;
};

Json to_json(const PublishedConclusion& c);
Json to_json(const PublicationState& s);

std::optional<PublicationState> publication_state(sql::Database& db, const std::string& id);
/** revision (内容ハッシュ) が現在の結論と一致するときだけ設定する。空の結論は公開できない。 */
bool set_publication(sql::Database& db, const std::string& id, bool publishable, const PublicationScope& scope,
                     const std::string& revision, std::int64_t now);
/** 公開中 (publishable かつ内容ハッシュ一致かつ結論が空でない) の結論。最大 200 件。 */
std::vector<PublishedConclusion> published_conclusions(sql::Database& db, const std::optional<std::string>& id = std::nullopt);

} // namespace di::integrations
