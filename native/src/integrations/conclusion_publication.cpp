#include "integrations/conclusion_publication.hpp"

#include "base/crypto.hpp"
#include "base/strings.hpp"

namespace di::integrations {

namespace {

/** sha256(JSON.stringify([title, conclusion, updated_at])) */
std::string content_hash(const sql::Row& row) {
    return crypto::sha256_hex(json_stringify(Json::array({row["title"].to_json(), row["conclusion"].to_json(), row["updated_at"].to_json()})));
}

bool non_blank(const sql::Row& row, const char* column) {
    auto v = row.opt_str(column);
    return v && !str::trim(*v).empty();
}

constexpr const char* kConclusionSql = "SELECT gap_id, title, conclusion, updated_at FROM conclusion_cache WHERE gap_id = ?";

}  // namespace

Json to_json(const PublishedConclusion& c) {
    Json o = Json::object();
    o["id"] = c.id;
    o["title"] = c.title;
    o["conclusion"] = c.conclusion;
    o["updatedAt"] = c.updatedAt;
    o["scope"] = c.scope;
    return o;
}

Json to_json(const PublicationState& s) {
    Json o = Json::object();
    o["publishable"] = s.publishable;
    o["scope"] = s.scope;
    o["revision"] = s.revision;
    o["title"] = s.title;
    o["conclusion"] = s.conclusion;
    return o;
}

std::optional<PublicationState> publication_state(sql::Database& db, const std::string& id) {
    auto row = db.get(kConclusionSql, {id});
    if (!row) return std::nullopt;
    auto setting = db.get("SELECT publishable, scope, content_hash FROM conclusion_publication WHERE conclusion_id = ?", {id});
    const std::string hash = content_hash(*row);
    PublicationState s;
    s.publishable = setting && json_truthy((*setting)["publishable"].to_json()) && setting->opt_str("content_hash") == hash;
    s.scope = setting && !setting->is_null("scope") ? setting->str("scope") : "private";
    s.revision = hash;
    s.title = row->str("title");
    s.conclusion = row->opt_str("conclusion").value_or("");
    return s;
}

bool set_publication(sql::Database& db, const std::string& id, bool publishable, const PublicationScope& scope,
                     const std::string& revision, std::int64_t now) {
    return db.transaction([&]() -> bool {
        auto row = db.get(kConclusionSql, {id});
        if (!row || content_hash(*row) != revision || (publishable && !non_blank(*row, "conclusion"))) return false;
        db.run(R"(INSERT INTO conclusion_publication VALUES (?, ?, ?, ?, ?)
      ON CONFLICT(conclusion_id) DO UPDATE SET publishable=excluded.publishable, scope=excluded.scope,
        content_hash=excluded.content_hash, updated_at=excluded.updated_at)",
               {id, publishable ? 1 : 0, scope, content_hash(*row), now});
        return true;
    });
}

std::vector<PublishedConclusion> published_conclusions(sql::Database& db, const std::optional<std::string>& id) {
    const std::string sql = std::string(R"(SELECT c.gap_id, c.title, c.conclusion, c.updated_at, p.scope, p.content_hash
    FROM conclusion_cache c JOIN conclusion_publication p ON p.conclusion_id=c.gap_id
    WHERE p.publishable=1 AND p.scope IN ('public','glab') )") +
                            (id ? "AND c.gap_id=?" : "") + R"(
    ORDER BY c.updated_at DESC, c.gap_id)";
    sql::Params params;
    if (id) params.add(*id);
    std::vector<PublishedConclusion> out;
    for (const auto& row : db.all(sql, params)) {
        if (!non_blank(row, "conclusion") || row.opt_str("content_hash") != content_hash(row)) continue;
        if (out.size() >= 200) break;
        out.push_back({row.str("gap_id"), row.str("title"), row.opt_str("conclusion").value_or(""), row["updated_at"].to_json(),
                       row.str("scope")});
    }
    return out;
}

} // namespace di::integrations
