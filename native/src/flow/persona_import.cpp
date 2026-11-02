#include "flow/persona_import.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

#include "base/crypto.hpp"
#include "base/strings.hpp"
#include "base/time.hpp"
#include "flow/db/connection.hpp"
#include "flow/sentiment_vector.hpp"
#include "visualize/locale_compare.hpp"

namespace di::flow {
namespace {

// @spec インポート (Vo → Di)
constexpr double SUPPORTED_VECTOR_SPEC_VERSION = 1;
constexpr double SUPPORTED_EXPORT_SPEC_VERSION = 2;
constexpr std::size_t MAX_TRAITS = 32;
constexpr std::int64_t MAX_TRAIT_LENGTH = 80;

[[noreturn]] void type_error(const std::string& message) { throw std::invalid_argument(message); }

bool is_hex_lower(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }
bool is_lower(char c) { return c >= 'a' && c <= 'z'; }
bool is_digit(char c) { return c >= '0' && c <= '9'; }

/** /^ext:voluptas:[0-9a-f]{16}$/ */
bool is_voluptas_user_id(const std::string& s) {
    const std::string prefix = "ext:voluptas:";
    if (s.size() != prefix.size() + 16 || !str::starts_with(s, prefix)) return false;
    return std::all_of(s.begin() + static_cast<std::ptrdiff_t>(prefix.size()), s.end(), is_hex_lower);
}

/** /^(mtg|style)\.[a-z][a-z0-9_]*$/ */
bool is_preference_axis(const std::string& s) {
    std::size_t i;
    if (str::starts_with(s, "mtg.")) i = 4;
    else if (str::starts_with(s, "style.")) i = 6;
    else return false;
    if (i >= s.size() || !is_lower(s[i])) return false;
    for (++i; i < s.size(); ++i)
        if (!is_lower(s[i]) && !is_digit(s[i]) && s[i] != '_') return false;
    return true;
}

/** /^[a-z0-9][a-z0-9/_-]{0,119}$/ */
bool is_mechanic_id(const std::string& s) {
    if (s.empty() || s.size() > 120) return false;
    if (!is_lower(s[0]) && !is_digit(s[0])) return false;
    for (std::size_t i = 1; i < s.size(); ++i) {
        const char c = s[i];
        if (!is_lower(c) && !is_digit(c) && c != '/' && c != '_' && c != '-') return false;
    }
    return true;
}

/** JS の String(v) (エラー文言用)。 */
std::string js_string(const Json& v) {
    if (v.is_string()) return v.get<std::string>();
    if (v.is_number()) return js_number_to_string(v.get<double>());
    if (v.is_boolean()) return v.get<bool>() ? "true" : "false";
    if (v.is_null()) return "null";
    if (v.is_array()) {
        std::vector<std::string> parts;
        for (const auto& x : v) parts.push_back(x.is_null() ? "" : js_string(x));
        return str::join(parts, ",");
    }
    return "[object Object]";
}

const Json& required_record(const Json& value) {
    if (!value.is_object()) type_error("persona import item must be an object");
    return value;
}

bool has(const Json& o, const char* key) { return o.contains(key); }

/** a ?? b (undefined / null は無いものとする)。 */
std::optional<Json> coalesce(const Json& o, const char* a, const char* b) {
    if (o.contains(a) && !o[a].is_null()) return o[a];
    if (o.contains(b)) return o[b];  // null ?? b → b (null を含む)
    return std::nullopt;
}

bool finite_number(const Json& v) { return v.is_number() && std::isfinite(v.get<double>()); }

std::string parse_pseudo_id(const std::optional<Json>& value) {
    if (!value || !value->is_string() || !is_voluptas_user_id(value->get<std::string>()))
        type_error("pseudoId must be a Voluptas HMAC pseudonym (ext:voluptas:<16 hex>)");
    return value->get<std::string>();
}

std::vector<double> parse_vector(const std::optional<Json>& value) {
    if (!value || !value->is_array() || value->size() != DIM)
        type_error("affectVector must contain exactly " + std::to_string(DIM) + " values");
    std::vector<double> out;
    for (const auto& item : *value) {
        if (!finite_number(item) || item.get<double>() < 0 || item.get<double>() > 1)
            type_error("affectVector values must be finite numbers in 0..1");
        out.push_back(item.get<double>());
    }
    return out;
}

std::vector<std::string> parse_traits(const Json& input) {
    if (!has(input, "traits")) return {};
    const Json& value = input["traits"];
    if (!value.is_array()) type_error("traits must be an array of strings");
    std::vector<std::string> traits;
    for (const auto& item : value) {
        if (!item.is_string()) type_error("traits must be an array of strings");
        traits.push_back(str::trim(item.get<std::string>()));
    }
    // 切り詰めてから重複除去する (先に除去すると切り詰めで重複が復活する)。
    std::vector<std::string> out;
    std::set<std::string> seen;
    for (const auto& t : traits) {
        if (t.empty()) continue;
        std::string cut = str::js_slice(t, 0, MAX_TRAIT_LENGTH);
        if (seen.insert(cut).second) out.push_back(std::move(cut));
    }
    if (out.size() > MAX_TRAITS) out.resize(MAX_TRAITS);
    return out;
}

std::vector<std::pair<std::string, double>> parse_preference_axes(const Json& input) {
    if (!has(input, "preferenceAxes")) return {};
    const Json& obj = required_record(input["preferenceAxes"]);
    std::vector<std::pair<std::string, double>> entries;
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        const Json& score = it.value();
        if (!is_preference_axis(it.key()) || !finite_number(score) || score.get<double>() < 0 || score.get<double>() > 1)
            type_error("preferenceAxes must contain canonical axis scores in 0..1");
        entries.emplace_back(it.key(), score.get<double>());
    }
    std::stable_sort(entries.begin(), entries.end(),
                     [](const auto& l, const auto& r) { return visualize::locale_compare(l.first, r.first) < 0; });
    return entries;
}

PersonaAttributes parse_attributes(const Json& input) {
    PersonaAttributes out;
    if (!has(input, "attributes")) return out;
    const Json& obj = required_record(input["attributes"]);
    for (const char* key : {"ageBand", "spending"}) {
        if (!obj.contains(key)) continue;
        const Json& item = obj[key];
        if (item.is_null() || (item.is_string() && item.get<std::string>().empty())) continue;
        if (!item.is_string() || str::js_length(item.get<std::string>()) > 80)
            type_error(std::string("attributes.") + key + " must be a string up to 80 characters");
        (std::string(key) == "ageBand" ? out.ageBand : out.spending) = str::trim(item.get<std::string>());
    }
    return out;
}

std::vector<PersonaAversion> parse_aversions(const Json& input) {
    if (!has(input, "aversions")) return {};
    const Json& value = input["aversions"];
    if (!value.is_array()) type_error("aversions must be an array");
    std::vector<PersonaAversion> out;
    for (const auto& item : value) {
        const Json& o = required_record(item);
        const Json target = o.contains("target") ? o["target"] : Json();
        const Json strength = o.contains("strength") ? o["strength"] : Json();
        if (!target.is_string() || str::trim(target.get<std::string>()).empty() || str::js_length(target.get<std::string>()) > 160 ||
            !finite_number(strength) || strength.get<double>() < 0 || strength.get<double>() > 1)
            type_error("aversions need a target and strength in 0..1");
        out.push_back({str::trim(target.get<std::string>()), strength.get<double>()});
    }
    return out;
}

std::vector<PersonaMechanicReaction> parse_mechanic_reactions(const Json& input) {
    if (!has(input, "mechanicReactions")) return {};
    const Json& value = input["mechanicReactions"];
    if (!value.is_array()) type_error("mechanicReactions must be an array");
    std::vector<PersonaMechanicReaction> out;
    for (const auto& item : value) {
        const Json& o = required_record(item);
        const Json id = o.contains("mechanicId") ? o["mechanicId"] : Json();
        const Json s = o.contains("sentiment") ? o["sentiment"] : Json();
        if (!id.is_string() || !is_mechanic_id(id.get<std::string>()) || !finite_number(s) || s.get<double>() < -2 ||
            s.get<double>() > 2)
            type_error("mechanicReactions need a mechanicId and sentiment in -2..2");
        out.push_back({id.get<std::string>(), s.get<double>()});
    }
    return out;
}

std::string anonymous_persona_id(const std::string& user_id) {
    return "persona:voluptas:" + crypto::sha256_hex(user_id).substr(0, 16);
}

std::string anonymous_persona_name(const std::string& user_id) { return "プレイヤー#" + crypto::sha256_hex(user_id).substr(0, 8); }

PoolPersona to_pool_persona(const VoluptasPersonaPayload& payload) {
    PoolPersona p;
    p.id = anonymous_persona_id(payload.pseudoId);
    p.name = anonymous_persona_name(payload.pseudoId);
    p.role = "opinion";
    p.traits = payload.traits;
    p.affectVector = payload.affectVector;
    p.origin = "imported";
    p.learningSource = "voluptas";
    if (payload.historyEvidence) {
        const auto& h = *payload.historyEvidence;
        p.label = "Voluptas 外部コメント由来 (" + js_number_to_string(h.sampleCount) + "件・" + js_number_to_string(h.observedMonths) +
                  "観測月・" + h.coverage + ")";
    } else {
        p.label = "Voluptas 匿名プロフィール";
    }
    p.sourceSpeakerId = "ext:feedback:" + payload.pseudoId;
    p.userId = payload.pseudoId;
    p.preferenceAxes = payload.preferenceAxes;
    p.attributes = payload.attributes;
    p.aversions = payload.aversions;
    p.mechanicReactions = payload.mechanicReactions;
    p.exportSpecVersion = payload.exportSpecVersion;
    return p;
}

void count_skip(PersonaImportSummary& summary, const std::string& reason) {
    summary.skipped += 1;
    for (auto& [k, v] : summary.skipReasons)
        if (k == reason) {
            v += 1;
            return;
        }
    summary.skipReasons.emplace_back(reason, 1);
}

} // namespace

Json to_json(const VoluptasPersonaPayload& p) {
    Json o = Json::object();
    o["pseudoId"] = p.pseudoId;
    o["affectVector"] = Json::array();
    for (double x : p.affectVector) o["affectVector"].push_back(x);
    o["vectorSpecVersion"] = p.vectorSpecVersion;
    if (p.exportSpecVersion) o["exportSpecVersion"] = *p.exportSpecVersion;
    o["traits"] = Json::array();
    for (const auto& t : p.traits) o["traits"].push_back(t);
    o["preferenceAxes"] = preference_axes_to_json(p.preferenceAxes);
    o["attributes"] = to_json(p.attributes);
    o["aversions"] = Json::array();
    for (const auto& a : p.aversions) o["aversions"].push_back(to_json(a));
    o["mechanicReactions"] = Json::array();
    for (const auto& m : p.mechanicReactions) o["mechanicReactions"].push_back(to_json(m));
    if (p.historyEvidence) o["historyEvidence"] = to_json(*p.historyEvidence);
    return o;
}

Json to_json(const PersonaImportSummary& s) {
    Json reasons = Json::object();
    for (const auto& [k, v] : s.skipReasons) reasons[k] = v;
    return Json{{"imported", s.imported}, {"inserted", s.inserted}, {"updated", s.updated},
                {"skipped", s.skipped},   {"skipReasons", reasons}, {"dryRun", s.dryRun}};
}

VoluptasPersonaPayload parse_voluptas_persona_payload(const Json& value) {
    return parse_voluptas_persona_payload(value, static_cast<double>(time::now_ms()));
}

VoluptasPersonaPayload parse_voluptas_persona_payload(const Json& value, double now_ms) {
    const Json& input = required_record(value);
    VoluptasPersonaPayload out;
    // parseVersionedPersonaCore
    if (input.contains("exportSpecVersion")) {
        const Json& v = input["exportSpecVersion"];
        if (!(v.is_number() && v.get<double>() == SUPPORTED_EXPORT_SPEC_VERSION))
            type_error("unsupported exportSpecVersion: " + js_string(v));
        out.exportSpecVersion = v.get<double>();
    }
    const auto vector_spec = coalesce(input, "vectorSpecVersion", "vector_spec_version");
    if (!(vector_spec && vector_spec->is_number() && vector_spec->get<double>() == SUPPORTED_VECTOR_SPEC_VERSION))
        type_error("unsupported vectorSpecVersion: " + (vector_spec ? js_string(*vector_spec) : std::string("undefined")));
    out.pseudoId = parse_pseudo_id(coalesce(input, "pseudoId", "user_id"));
    out.affectVector = parse_vector(coalesce(input, "affectVector", "affect_vector"));
    out.vectorSpecVersion = vector_spec->get<double>();
    // parsePersonaCompartments
    out.traits = parse_traits(input);
    out.preferenceAxes = parse_preference_axes(input);
    out.attributes = parse_attributes(input);
    out.aversions = parse_aversions(input);
    out.mechanicReactions = parse_mechanic_reactions(input);
    out.historyEvidence = parse_history_evidence(input.contains("historyEvidence") ? std::optional<Json>(input["historyEvidence"])
                                                                                    : std::nullopt,
                                                 now_ms);
    return out;
}

Json parse_voluptas_persona_batch(const Json& value) {
    const Json candidate = value.is_object() ? (value.contains("personas") ? value["personas"] : Json()) : value;
    if (!candidate.is_array()) type_error("persona import payload must be an array or { personas: [] }");
    return candidate;
}

std::string persona_import_reason_code(const std::string& message) {
    if (str::contains(message, "exportSpecVersion")) return "export-spec";
    if (str::contains(message, "vectorSpecVersion")) return "vector-spec";
    if (str::contains(message, "affectVector")) return "affect-vector";
    if (str::contains(message, "pseudoId")) return "pseudo-id";
    return "invalid-payload";
}

PersonaImportSummary import_voluptas_personas(const Json& value, const ImportVoluptasOptions& options) {
    const Json candidates = parse_voluptas_persona_batch(value);
    PersonaImportSummary summary;
    summary.skipped = options.invalidJsonLines;
    if (options.invalidJsonLines > 0) summary.skipReasons.emplace_back("invalid-json", options.invalidJsonLines);
    summary.dryRun = options.dryRun;
    const double now = static_cast<double>(time::now_ms());
    for (const auto& candidate : candidates) {
        VoluptasPersonaPayload payload;
        try {
            payload = parse_voluptas_persona_payload(candidate, now);
        } catch (const std::exception& e) {
            count_skip(summary, persona_import_reason_code(e.what()));
            continue;
        }
        // アーカイブ済みの同一人物も「更新」扱い (upsert が復帰させる)。
        const bool exists = find_pool_persona_by_user_id(payload.pseudoId, true).has_value();
        try {
            if (!summary.dryRun) {
                auto db = db::get_flow_db();
                db->transaction([&] {
                    upsert_pool_persona_by_user_id(to_pool_persona(payload));
                    save_history_evidence(*db, payload.pseudoId, payload.historyEvidence);
                });
            }
        } catch (const std::exception&) {
            // 1 行の書き込み失敗で残りの取込を落とさない (spec persona-bridge §1.3)。
            count_skip(summary, "write-failed");
            continue;
        }
        if (exists) summary.updated += 1;
        else summary.inserted += 1;
        summary.imported += 1;
    }
    return summary;
}

} // namespace di::flow
