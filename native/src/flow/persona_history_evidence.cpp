#include "flow/persona_history_evidence.hpp"

#include <cmath>
#include <set>
#include <stdexcept>

namespace di::flow {

namespace {

const std::set<std::string>& allowed_keys() {
    static const std::set<std::string> keys = {"schemaVersion", "source", "generatedAt", "expiresAt", "windowStart",
                                               "firstObservedAt", "lastObservedAt", "sampleCount", "observedMonths",
                                               "coverage"};
    return keys;
}

[[noreturn]] void invalid(const std::string& path, const std::string& message) {
    throw std::invalid_argument("history evidence: " + path + ": " + message);
}

double finite(const Json& o, const char* key) {
    auto it = o.find(key);
    if (it == o.end() || !it->is_number() || !std::isfinite(it->get<double>())) invalid(key, "Expected finite number");
    return it->get<double>();
}

double integer(const Json& o, const char* key) {
    const double v = finite(o, key);
    if (v != std::floor(v)) invalid(key, "Expected integer");
    return v;
}

std::string one_of(const Json& o, const char* key, std::initializer_list<const char*> values) {
    auto it = o.find(key);
    if (it != o.end() && it->is_string())
        for (const char* v : values)
            if (it->get_ref<const std::string&>() == v) return *it;
    invalid(key, "Invalid enum value");
}

} // namespace

std::optional<PersonaHistoryEvidence> parse_history_evidence(const std::optional<Json>& value, double now_ms) {
    if (!value) return std::nullopt;
    const Json& o = *value;
    if (!o.is_object()) invalid("", "Expected object");
    for (auto it = o.begin(); it != o.end(); ++it)
        if (!allowed_keys().count(it.key())) invalid(it.key(), "Unrecognized key");  // .strict()
    PersonaHistoryEvidence e;
    auto sv = o.find("schemaVersion");
    if (sv == o.end() || !sv->is_number() || sv->get<double>() != 1) invalid("schemaVersion", "Invalid literal value");
    e.source = one_of(o, "source", {"steam", "youtube"});
    e.generatedAt = finite(o, "generatedAt");
    e.expiresAt = finite(o, "expiresAt");
    e.windowStart = finite(o, "windowStart");
    e.firstObservedAt = finite(o, "firstObservedAt");
    e.lastObservedAt = finite(o, "lastObservedAt");
    e.sampleCount = integer(o, "sampleCount");
    if (e.sampleCount <= 0) invalid("sampleCount", "Number must be greater than 0");
    e.observedMonths = integer(o, "observedMonths");
    if (e.observedMonths < 1 || e.observedMonths > 7) invalid("observedMonths", "Number must be between 1 and 7");
    e.coverage = one_of(o, "coverage", {"partial", "target-window-observed"});
    if (e.expiresAt <= now_ms || e.generatedAt > now_ms || e.expiresAt <= e.generatedAt ||
        e.expiresAt > e.generatedAt + 30.0 * 86400000.0 || e.windowStart > e.firstObservedAt ||
        e.firstObservedAt > e.lastObservedAt || e.lastObservedAt > e.generatedAt)
        throw std::runtime_error("Invalid or expired history evidence");
    return e;
}

Json to_json(const PersonaHistoryEvidence& e) {
    Json o = Json::object();
    o["schemaVersion"] = e.schemaVersion;
    o["source"] = e.source;
    o["generatedAt"] = e.generatedAt;
    o["expiresAt"] = e.expiresAt;
    o["windowStart"] = e.windowStart;
    o["firstObservedAt"] = e.firstObservedAt;
    o["lastObservedAt"] = e.lastObservedAt;
    o["sampleCount"] = e.sampleCount;
    o["observedMonths"] = e.observedMonths;
    o["coverage"] = e.coverage;
    return o;
}

void save_history_evidence(sql::Database& db, const std::string& user_id, const std::optional<PersonaHistoryEvidence>& evidence) {
    if (!evidence) return;
    db.run(
        "INSERT INTO flow_persona_history VALUES(?,?,?)\n"
        "    ON CONFLICT(user_id) DO UPDATE SET expires_at=excluded.expires_at,payload=excluded.payload",
        {user_id, evidence->expiresAt, json_stringify(to_json(*evidence))});
}

void purge_history_personas(sql::Database& db, double now_ms) {
    if (!db.get("SELECT 1 FROM flow_persona_history WHERE expires_at<=? LIMIT 1", {now_ms})) return;
    db.transaction([&] {
        db.run(
            "DELETE FROM flow_persona WHERE user_id IN\n"
            "      (SELECT user_id FROM flow_persona_history WHERE expires_at<=?)",
            {now_ms});
        db.run("DELETE FROM flow_persona_history WHERE expires_at<=?", {now_ms});
    });
}

} // namespace di::flow
