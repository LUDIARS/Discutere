#include "flow/proxy/store.hpp"

#include <stdexcept>

#include "base/sqlite.hpp"
#include "base/strings.hpp"
#include "flow/db/connection.hpp"

namespace di::flow::proxy {
namespace {

const sql::QueryLimits kListLimits{100'000, 512u << 20};
const JsonLimits kPackLimits{kProxyPackJsonMaxBytes, 100'000, 32};

PersonaPack parse_stored_pack(const std::string& text) {
    std::string error;
    auto parsed = parse_json(text, kPackLimits, &error);
    if (!parsed) throw std::runtime_error("flow_proxy_profile.pack_json を読めません: " + error);
    auto v = parse_persona_pack(*parsed);
    if (!v.ok || !v.pack) throw std::runtime_error("flow_proxy_profile.pack_json が契約に通りません");
    return *v.pack;
}

ProxyProfileRow to_row(const sql::Row& r) {
    ProxyProfileRow out;
    out.speakerKey = r.str("speaker_key");
    out.genre = r.str("genre");
    out.label = r.str("label");
    out.profileVersion = r.str("profile_version");
    out.contentHash = r.str("content_hash");
    out.pack = parse_stored_pack(r.str("pack_json"));
    out.confidence = r.str("confidence");
    out.expiresAt = r.num("expires_at");
    out.revoked = r.i64("revoked") == 1;
    out.evalStatus = r.str("eval_status");
    out.evalJson = r.opt_str("eval_json");
    out.importedAt = r.num("imported_at");
    out.updatedAt = r.num("updated_at");
    return out;
}

std::vector<ProxyProfileRow> rows_of(const std::vector<sql::Row>& rows) {
    std::vector<ProxyProfileRow> out;
    for (const auto& r : rows) out.push_back(to_row(r));
    return out;
}

} // namespace

Json to_json(const ProxyProfileRow& r) {
    return Json{{"speakerKey", r.speakerKey},     {"genre", r.genre},         {"label", r.label},
                {"profileVersion", r.profileVersion}, {"contentHash", r.contentHash}, {"pack", to_json(r.pack)},
                {"confidence", r.confidence},     {"expiresAt", r.expiresAt}, {"revoked", r.revoked},
                {"evalStatus", r.evalStatus},     {"evalJson", r.evalJson ? Json(*r.evalJson) : Json(nullptr)},
                {"importedAt", r.importedAt},     {"updatedAt", r.updatedAt}};
}

std::optional<ProxyProfileRow> get_proxy_profile(const std::string& speaker_key, const std::string& genre) {
    auto row = db::get_flow_db()->get("SELECT * FROM flow_proxy_profile WHERE speaker_key = ? AND genre = ?", {speaker_key, genre});
    if (!row) return std::nullopt;
    return to_row(*row);
}

std::string upsert_proxy_profile(const UpsertProxyProfileInput& input) {
    const auto& pack = input.pack;
    const auto existing = get_proxy_profile(pack.speakerKey, pack.genre);
    if (existing && existing->contentHash == input.contentHash && !existing->revoked) return "unchanged";
    auto db = db::get_flow_db();
    const std::string pack_json = json_stringify(to_json(pack));
    if (!existing) {
        db->run(
            "INSERT INTO flow_proxy_profile\n"
            "         (speaker_key, genre, label, profile_version, content_hash, pack_json, confidence, expires_at,\n"
            "          revoked, eval_status, eval_json, imported_at, updated_at)\n"
            "       VALUES (?, ?, ?, ?, ?, ?, ?, ?, 0, 'unevaluated', NULL, ?, ?)",
            {pack.speakerKey, pack.genre, input.label, pack.profileVersion, input.contentHash, pack_json, pack.sufficiency.confidence,
             pack.expiresAt, input.now, input.now});
        return "inserted";
    }
    db->run(
        "UPDATE flow_proxy_profile\n"
        "        SET label = ?, profile_version = ?, content_hash = ?, pack_json = ?, confidence = ?, expires_at = ?,\n"
        "            revoked = 0, eval_status = 'unevaluated', eval_json = NULL, updated_at = ?\n"
        "      WHERE speaker_key = ? AND genre = ?",
        {input.label, pack.profileVersion, input.contentHash, pack_json, pack.sufficiency.confidence, pack.expiresAt, input.now,
         pack.speakerKey, pack.genre});
    return "updated";
}

bool revoke_proxy_profile(const std::string& speaker_key, const std::string& genre, double now) {
    const auto r = db::get_flow_db()->run(
        "UPDATE flow_proxy_profile SET revoked = 1, updated_at = ? WHERE speaker_key = ? AND genre = ? AND revoked = 0",
        {now, speaker_key, genre});
    return r.changes > 0;
}

void set_proxy_eval_status(const std::string& speaker_key, const std::string& genre, const ProxyEvalStatus& status,
                           const std::string& eval_json, double now) {
    db::get_flow_db()->run(
        "UPDATE flow_proxy_profile SET eval_status = ?, eval_json = ?, updated_at = ? WHERE speaker_key = ? AND genre = ?",
        {status, eval_json, now, speaker_key, genre});
}

std::optional<std::string> is_castable(const ProxyProfileRow& row, double now, bool require_passed) {
    if (row.revoked) return "失効している";
    if (row.expiresAt <= now) return "有効期限が切れている";
    if (row.confidence == "low") return "材料が足りない (confidence=low)";
    if (row.evalStatus == "failed") return "本人らしさの評価に合格していない";
    if (require_passed && row.evalStatus != "passed") return "本人らしさの評価がまだ";
    return std::nullopt;
}

std::vector<ProxyProfileRow> find_proxy_profiles_by_label(const std::string& label, const std::optional<std::string>& genre) {
    auto db = db::get_flow_db();
    if (genre && !genre->empty())
        return rows_of(db->all("SELECT * FROM flow_proxy_profile WHERE label = ? AND genre = ? ORDER BY updated_at DESC", {label, *genre},
                               kListLimits));
    return rows_of(db->all("SELECT * FROM flow_proxy_profile WHERE label = ? ORDER BY updated_at DESC", {label}, kListLimits));
}

std::vector<ProxyProfileRow> list_proxy_profiles(const ListProxyProfilesOpts& opts) {
    std::vector<std::string> where;
    sql::Params params;
    if (opts.genre && !opts.genre->empty()) {
        where.push_back("genre = ?");
        params.add(*opts.genre);
    }
    if (!opts.includeRevoked) where.push_back("revoked = 0");
    const std::string sql =
        "SELECT * FROM flow_proxy_profile" + (where.empty() ? std::string() : " WHERE " + str::join(where, " AND ")) + " ORDER BY genre, label";
    return rows_of(db::get_flow_db()->all(sql, params, kListLimits));
}

} // namespace di::flow::proxy
