#include "flow/persona_pool.hpp"

#include <algorithm>
#include <stdexcept>

#include "base/sqlite.hpp"
#include "base/time.hpp"
#include "base/strings.hpp"
#include "flow/db/connection.hpp"
#include "flow/persona_descriptor.hpp"
#include "flow/persona_history_evidence.hpp"
#include "flow/sentiment_vector.hpp"

namespace di::flow {
namespace {

// 毎回 purge してから返す (期限切れの派生プロフィールを議論に残さない。TS の getFlowDb ラッパと同じ)。
sql::DatabasePtr pool_db() {
    auto db = db::get_flow_db();
    purge_history_personas(*db, static_cast<double>(time::now_ms()));
    return db;
}

/** DB の JSON 列の上限 (外部入力として課金する)。 */
const JsonLimits kColumnLimits{kPersonaJsonColumnMaxBytes, 100'000, 16};
/** 一覧の上限 (行数・バイト数)。 */
const sql::QueryLimits kListLimits{200'000, 256u << 20};

std::optional<Json> parse_column(const std::optional<std::string>& s) {
    if (!s) return std::nullopt;
    return parse_json(*s, kColumnLimits);
}

/** parseJsonArray: 配列でなければ空。 */
Json parse_json_array(const std::optional<std::string>& s) {
    auto v = parse_column(s);
    return v && v->is_array() ? *v : Json::array();
}

/** parseJsonObject: object でなければ空。 */
Json parse_json_object(const std::optional<std::string>& s) {
    auto v = parse_column(s);
    return v && v->is_object() ? *v : Json::object();
}

std::vector<std::string> strings_of(const Json& arr) {
    std::vector<std::string> out;
    for (const auto& v : arr)
        if (v.is_string()) out.push_back(v.get<std::string>());
    return out;
}

std::vector<double> numbers_of(const Json& arr) {
    std::vector<double> out;
    for (const auto& v : arr)
        if (v.is_number()) out.push_back(v.get<double>());
    return out;
}

PoolPersona row_to_persona(const sql::Row& r) {
    PoolPersona p;
    p.id = r.str("id");
    p.name = r.str("name");
    p.role = r.str("role");
    p.speechStyle = r.str("speech_style");
    p.traits = strings_of(parse_json_array(r.opt_str("traits_json")));
    p.affectVector = numbers_of(parse_json_array(r.opt_str("affect_vector_json")));
    p.origin = r.str("origin");
    p.parentIds = strings_of(parse_json_array(r.opt_str("parent_ids_json")));
    p.learningSource = r.opt_str("learning_source");
    p.label = r.opt_str("label");
    p.model = r.opt_str("model");
    p.sourceSpeakerId = r.opt_str("source_speaker_id");
    p.userId = r.opt_str("user_id");
    p.typicality = r.opt_num("typicality");
    p.polarityBias = r.opt_num("polarity_bias");
    p.affectDispersion = r.opt_num("affect_dispersion");
    const Json axes = parse_json_object(r.opt_str("preference_axes_json"));
    for (auto it = axes.begin(); it != axes.end(); ++it)
        if (it.value().is_number()) p.preferenceAxes.emplace_back(it.key(), it.value().get<double>());
    const Json attrs = parse_json_object(r.opt_str("attributes_json"));
    p.attributes.ageBand = json_opt_string(attrs, "ageBand");
    p.attributes.spending = json_opt_string(attrs, "spending");
    for (const auto& a : parse_json_array(r.opt_str("aversions_json"))) {
        if (!a.is_object()) continue;
        p.aversions.push_back({json_string(a, "target"), json_number(a, "strength")});
    }
    for (const auto& m : parse_json_array(r.opt_str("mechanic_reactions_json"))) {
        if (!m.is_object()) continue;
        p.mechanicReactions.push_back({json_string(m, "mechanicId"), json_number(m, "sentiment")});
    }
    p.exportSpecVersion = r.opt_num("export_spec_version");
    return p;
}

std::string stringify_strings(const std::vector<std::string>& v) {
    Json a = Json::array();
    for (const auto& s : v) a.push_back(s);
    return json_stringify(a);
}

Json vector_to_json_any(const std::vector<double>& v) {
    Json a = Json::array();
    for (double x : v) a.push_back(x);
    return a;
}

std::string stringify_numbers(const std::vector<double>& v) { return json_stringify(vector_to_json_any(v)); }

std::string stringify_aversions(const std::vector<PersonaAversion>& v) {
    Json a = Json::array();
    for (const auto& x : v) a.push_back(to_json(x));
    return json_stringify(a);
}

std::string stringify_reactions(const std::vector<PersonaMechanicReaction>& v) {
    Json a = Json::array();
    for (const auto& x : v) a.push_back(to_json(x));
    return json_stringify(a);
}

std::optional<PoolPersona> get_one(const std::string& sql, const sql::Params& params) {
    auto row = pool_db()->get(sql, params);
    if (!row) return std::nullopt;
    return row_to_persona(*row);
}

} // namespace

Json preference_axes_to_json(const std::vector<std::pair<std::string, double>>& axes) {
    Json o = Json::object();
    for (const auto& [k, v] : axes) o[k] = v;
    return o;
}

Json to_json(const PersonaAttributes& a) {
    Json o = Json::object();
    if (a.ageBand) o["ageBand"] = *a.ageBand;
    if (a.spending) o["spending"] = *a.spending;
    return o;
}

Json to_json(const PersonaAversion& a) { return Json{{"target", a.target}, {"strength", a.strength}}; }

Json to_json(const PersonaMechanicReaction& r) { return Json{{"mechanicId", r.mechanicId}, {"sentiment", r.sentiment}}; }

Json to_json(const PoolPersona& p) {
    Json o = Json::object();
    o["id"] = p.id;
    o["name"] = p.name;
    o["role"] = p.role;
    o["speechStyle"] = p.speechStyle;
    o["traits"] = Json::array();
    for (const auto& t : p.traits) o["traits"].push_back(t);
    o["affectVector"] = vector_to_json_any(p.affectVector);
    o["origin"] = p.origin;
    o["parentIds"] = Json::array();
    for (const auto& t : p.parentIds) o["parentIds"].push_back(t);
    if (p.learningSource) o["learningSource"] = *p.learningSource;
    if (p.label) o["label"] = *p.label;
    if (p.model) o["model"] = *p.model;
    if (p.sourceSpeakerId) o["sourceSpeakerId"] = *p.sourceSpeakerId;
    if (p.userId) o["userId"] = *p.userId;
    if (p.typicality) o["typicality"] = *p.typicality;
    if (p.polarityBias) o["polarityBias"] = *p.polarityBias;
    if (p.affectDispersion) o["affectDispersion"] = *p.affectDispersion;
    o["preferenceAxes"] = preference_axes_to_json(p.preferenceAxes);
    o["attributes"] = to_json(p.attributes);
    o["aversions"] = Json::array();
    for (const auto& a : p.aversions) o["aversions"].push_back(to_json(a));
    o["mechanicReactions"] = Json::array();
    for (const auto& m : p.mechanicReactions) o["mechanicReactions"].push_back(to_json(m));
    if (p.exportSpecVersion) o["exportSpecVersion"] = *p.exportSpecVersion;
    return o;
}

void insert_pool_persona(const PoolPersona& p) {
    if (p.affectVector.size() != DIM)
        throw std::invalid_argument("affectVector dim must be " + std::to_string(DIM) + ", got " + std::to_string(p.affectVector.size()));
    pool_db()->run(
        "INSERT INTO flow_persona\n"
        "       (id, name, role, speech_style, traits_json, affect_vector_json, origin, parent_ids_json,\n"
        "        learning_source, label, model, source_speaker_id, user_id, typicality, polarity_bias, affect_dispersion,\n"
        "        preference_axes_json, attributes_json, aversions_json, mechanic_reactions_json,\n"
        "        export_spec_version, archived, created_at)\n"
        "     VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 0, ?)",
        {p.id, p.name, p.role, p.speechStyle, stringify_strings(p.traits), stringify_numbers(p.affectVector), p.origin,
         stringify_strings(p.parentIds), sql::Value(p.learningSource), sql::Value(p.label), sql::Value(p.model),
         sql::Value(p.sourceSpeakerId), sql::Value(p.userId), sql::Value(p.typicality), sql::Value(p.polarityBias),
         sql::Value(p.affectDispersion), json_stringify(preference_axes_to_json(p.preferenceAxes)),
         json_stringify(to_json(p.attributes)), stringify_aversions(p.aversions), stringify_reactions(p.mechanicReactions),
         sql::Value(p.exportSpecVersion), static_cast<std::int64_t>(time::now_ms())});
}

std::optional<PoolPersona> find_pool_persona_by_user_id(const std::string& user_id, bool include_archived) {
    return get_one(std::string("SELECT * FROM flow_persona WHERE user_id = ?") + (include_archived ? "" : " AND archived = 0") +
                       "\n        ORDER BY created_at ASC LIMIT 1",
                   {user_id});
}

PoolPersona upsert_pool_persona_by_user_id(const PoolPersona& persona) {
    if (!persona.userId) throw std::invalid_argument("upsertPoolPersonaByUserId: userId required");
    auto existing = find_pool_persona_by_user_id(*persona.userId, true);
    if (!existing && persona.sourceSpeakerId) existing = find_pool_persona_by_speaker(*persona.sourceSpeakerId);
    if (!existing) {
        insert_pool_persona(persona);
        return persona;
    }
    std::optional<std::string> speaker = persona.sourceSpeakerId ? persona.sourceSpeakerId : existing->sourceSpeakerId;
    pool_db()->run(
        "UPDATE flow_persona SET name = ?, role = ?, speech_style = ?, traits_json = ?,\n"
        "         affect_vector_json = ?, origin = 'imported', learning_source = ?, label = ?, model = ?,\n"
        "         source_speaker_id = ?, user_id = ?, preference_axes_json = ?, attributes_json = ?,\n"
        "         aversions_json = ?, mechanic_reactions_json = ?, export_spec_version = ?,\n"
        "         archived = 0 WHERE id = ?",
        {persona.name, persona.role, persona.speechStyle, stringify_strings(persona.traits), stringify_numbers(persona.affectVector),
         persona.learningSource.value_or("voluptas"), sql::Value(persona.label), sql::Value(persona.model), sql::Value(speaker),
         *persona.userId, json_stringify(preference_axes_to_json(persona.preferenceAxes)), json_stringify(to_json(persona.attributes)),
         stringify_aversions(persona.aversions), stringify_reactions(persona.mechanicReactions), sql::Value(persona.exportSpecVersion),
         existing->id});
    PoolPersona out = persona;
    out.id = existing->id;
    return out;
}

std::optional<PoolPersona> find_pool_persona_by_speaker(const std::string& source_speaker_id) {
    return get_one("SELECT * FROM flow_persona WHERE source_speaker_id = ? AND archived = 0 ORDER BY created_at ASC LIMIT 1",
                   {source_speaker_id});
}

PoolPersona upsert_pool_persona_by_speaker(const PoolPersona& p) {
    if (!p.sourceSpeakerId) throw std::invalid_argument("upsertPoolPersonaBySpeaker: sourceSpeakerId 必須");
    auto existing = find_pool_persona_by_speaker(*p.sourceSpeakerId);
    if (!existing) {
        insert_pool_persona(p);
        return p;
    }
    if (existing->origin == "imported" && existing->userId) {
        // Voluptas の profile が正本。レビュー採用ではベクトルを置き換えない。
        pool_db()->run(
            "UPDATE flow_persona SET source_speaker_id = ?, typicality = ?, polarity_bias = ?,\n"
            "           affect_dispersion = ? WHERE id = ?",
            {*p.sourceSpeakerId, sql::Value(p.typicality), sql::Value(p.polarityBias), sql::Value(p.affectDispersion), existing->id});
        PoolPersona out = *existing;
        out.sourceSpeakerId = p.sourceSpeakerId;
        out.typicality = p.typicality;
        out.polarityBias = p.polarityBias;
        out.affectDispersion = p.affectDispersion;
        return out;
    }
    pool_db()->run(
        "UPDATE flow_persona SET name=?, role=?, speech_style=?, traits_json=?, affect_vector_json=?,\n"
        "         origin=?, learning_source=?, label=?, typicality=?, polarity_bias=?, affect_dispersion=? WHERE id=?",
        {p.name, p.role, p.speechStyle, stringify_strings(p.traits), stringify_numbers(p.affectVector), p.origin,
         sql::Value(p.learningSource), sql::Value(p.label), sql::Value(p.typicality), sql::Value(p.polarityBias),
         sql::Value(p.affectDispersion), existing->id});
    PoolPersona out = p;
    out.id = existing->id;
    return out;
}

void set_pool_persona_typicality(const std::string& id, double typicality) {
    pool_db()->run("UPDATE flow_persona SET typicality = ? WHERE id = ?", {typicality, id});
}

std::optional<PoolPersona> get_pool_persona(const std::string& id) {
    return get_one("SELECT * FROM flow_persona WHERE id = ? AND archived = 0", {id});
}

std::vector<PoolPersona> list_pool_personas(const PoolPersonaFilter& filter) {
    std::vector<std::string> where{"archived = 0"};
    sql::Params params;
    if (filter.origin && !filter.origin->empty()) {
        where.push_back("origin = ?");
        params.add(*filter.origin);
    }
    if (filter.learningSource && !filter.learningSource->empty()) {
        where.push_back("learning_source = ?");
        params.add(*filter.learningSource);
    }
    std::vector<PoolPersona> out;
    for (const auto& r : pool_db()->all("SELECT * FROM flow_persona WHERE " + str::join(where, " AND ") + " ORDER BY created_at ASC",
                                        params, kListLimits))
        out.push_back(row_to_persona(r));
    return out;
}

std::optional<PoolPersona> find_pool_persona(const std::string& id_or_name) {
    if (auto by_id = get_pool_persona(id_or_name)) return by_id;
    const std::string key = str::trim(id_or_name);
    if (key.empty()) return std::nullopt;
    return get_one("SELECT * FROM flow_persona WHERE name = ? AND archived = 0 ORDER BY created_at ASC LIMIT 1", {key});
}

void archive_pool_persona(const std::string& id) { pool_db()->run("UPDATE flow_persona SET archived = 1 WHERE id = ?", {id}); }

std::vector<AffinityHit> select_by_affinity(const std::vector<double>& vector, int k,
                                            const std::optional<std::vector<PoolPersona>>& candidates) {
    const std::vector<PoolPersona> pool = candidates ? *candidates : list_pool_personas();
    std::vector<AffinityHit> hits;
    hits.reserve(pool.size());
    for (const auto& persona : pool) hits.push_back({persona, cosine(vector, persona.affectVector)});
    std::stable_sort(hits.begin(), hits.end(), [](const AffinityHit& a, const AffinityHit& b) { return b.similarity < a.similarity; });
    hits.resize(std::min(hits.size(), static_cast<std::size_t>(std::max(0, k))));
    return hits;
}

std::vector<AffinityHit> select_possession_by_theme(const std::string& theme, int k,
                                                    const std::optional<std::vector<PoolPersona>>& candidates) {
    return select_by_affinity(text_to_vector(theme), k, candidates);
}

FlowPersona to_flow_persona(const PoolPersona& p, const ToFlowPersonaOpts& opts) {
    const FlowRole role = opts.role ? *opts.role : p.role;
    FlowPersona out;
    out.id = p.id;
    out.name = p.name;
    out.role = role;
    // stance はセッション固定 (respec 01)。指定が無ければロール既定 (debater は pro 起点)。
    out.stance = opts.stance ? *opts.stance : role_default_stance(role);
    out.speechStyle = p.speechStyle;
    out.traits = p.traits;
    out.model = p.model && !p.model->empty() ? *p.model : opts.defaultModel;
    out.isLocal = opts.isLocal;
    return out;
}

std::string describe_possession(const PoolPersona& p) { return build_persona_descriptor(p); }

} // namespace di::flow
