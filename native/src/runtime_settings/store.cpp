#include "runtime_settings/store.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>

#include "base/strings.hpp"
#include "runtime_settings/js_value.hpp"

namespace di::runtime_settings {
namespace {

const std::string kFacilitatorKey = "facilitator";
const std::string kRolePrefix = "rolePrompt:";
const std::string kRulePrefix = "ruleInstruction:";
const std::string kCostRelayKey = "costRelay";

/** DB から読む override の上限 (値は小さな JSON のはず)。 */
const JsonLimits kOverrideLimits{256u * 1024, 10'000, 16};

const Json* field(const Json& object, const char* key) {
    if (!object.is_object()) return nullptr;
    auto it = object.find(key);
    return it == object.end() ? nullptr : &*it;
}

double clamp_int(const Json* v, double min, double fallback) {
    // const n = typeof v === "number" ? v : Number(v); undefined は NaN
    const double n = v ? js::to_number(*v) : std::nan("");
    if (!std::isfinite(n)) return fallback;
    return std::max(min, js::round(n));
}

std::string normalize_policy(const Json* v, const std::string& fallback) {
    if (v && v->is_string()) {
        const auto& s = v->get_ref<const std::string&>();
        if (s == "default" || s == "aufhebung-only") return s;
    }
    return fallback;
}

/** JS の Object.keys の順 (配列の添字形のキーを昇順で先に、残りは挿入順)。同名は後の値で上書き。 */
std::vector<std::pair<std::string, std::string>> js_object_order(const std::vector<std::pair<std::string, std::string>>& in) {
    std::vector<std::pair<std::string, std::string>> uniq;
    for (const auto& [k, v] : in) {
        auto it = std::find_if(uniq.begin(), uniq.end(), [&](const auto& p) { return p.first == k; });
        if (it != uniq.end()) it->second = v;
        else uniq.emplace_back(k, v);
    }
    auto index_of = [](const std::string& k) -> std::optional<std::uint64_t> {
        if (k.empty() || k.size() > 10) return std::nullopt;
        if (k.size() > 1 && k[0] == '0') return std::nullopt;
        std::uint64_t n = 0;
        for (char c : k) {
            if (c < '0' || c > '9') return std::nullopt;
            n = n * 10 + static_cast<std::uint64_t>(c - '0');
        }
        if (n >= 4294967295ull) return std::nullopt;
        return n;
    };
    std::vector<std::pair<std::uint64_t, std::pair<std::string, std::string>>> indices;
    std::vector<std::pair<std::string, std::string>> rest;
    for (auto& p : uniq) {
        if (auto n = index_of(p.first)) indices.emplace_back(*n, p);
        else rest.push_back(p);
    }
    std::stable_sort(indices.begin(), indices.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<std::pair<std::string, std::string>> out;
    for (auto& [n, p] : indices) out.push_back(p);
    for (auto& p : rest) out.push_back(p);
    return out;
}

/** { ...cur, ...patch } (object 以外の cur / patch は鍵を持たない)。 */
Json spread(const Json& cur, const Json& patch) {
    Json out = Json::object();
    if (cur.is_object())
        for (auto it = cur.begin(); it != cur.end(); ++it) out[it.key()] = it.value();
    if (patch.is_object())
        for (auto it = patch.begin(); it != patch.end(); ++it) out[it.key()] = it.value();
    return out;
}

class SqliteRuntimeSettingsStore final : public RuntimeSettingsStore {
public:
    SqliteRuntimeSettingsStore(sql::DatabasePtr db, RuntimeSettingsDefaults defaults)
        : db_(std::move(db)), defaults_(std::move(defaults)) {
        defaults_.rolePrompts = js_object_order(defaults_.rolePrompts);
        defaults_.ruleInstructions = js_object_order(defaults_.ruleInstructions);
        db_->exec(
            "CREATE TABLE IF NOT EXISTS runtime_settings (\n"
            "       key        TEXT PRIMARY KEY,\n"
            "       value      TEXT NOT NULL,\n"
            "       updated_at INTEGER NOT NULL\n"
            "     )");
        load_all();
    }

    FacilitatorTuning get_facilitator_tuning() override {
        std::lock_guard lock(mutex_);
        return merge_facilitator_tuning(defaults_.facilitator, parse_override(kFacilitatorKey));
    }

    FacilitatorTuning set_facilitator_tuning(const Json& patch) override {
        std::lock_guard lock(mutex_);
        const auto merged = merge_facilitator_tuning(defaults_.facilitator, spread(parse_override(kFacilitatorKey), patch));
        // 実効値そのものを override として保存 (常に完全な tuning を持たせる)。
        put(kFacilitatorKey, json_stringify(to_json(merged)));
        return merged;
    }

    std::string get_role_prompt(const std::string& role) override {
        std::lock_guard lock(mutex_);
        return lookup(kRolePrefix + role, defaults_.rolePrompts, role);
    }
    std::vector<OverrideView> list_role_prompts() override {
        std::lock_guard lock(mutex_);
        return views(kRolePrefix, defaults_.rolePrompts);
    }
    void set_role_prompt(const std::string& role, const std::optional<std::string>& text) override {
        std::lock_guard lock(mutex_);
        set_text(kRolePrefix + role, text);
    }

    std::string get_rule_instruction(const std::string& rule_id) override {
        std::lock_guard lock(mutex_);
        return lookup(kRulePrefix + rule_id, defaults_.ruleInstructions, rule_id);
    }
    std::vector<OverrideView> list_rule_instructions() override {
        std::lock_guard lock(mutex_);
        return views(kRulePrefix, defaults_.ruleInstructions);
    }
    void set_rule_instruction(const std::string& rule_id, const std::optional<std::string>& text) override {
        std::lock_guard lock(mutex_);
        set_text(kRulePrefix + rule_id, text);
    }

    CostRelaySettings get_cost_relay() override {
        std::lock_guard lock(mutex_);
        return merge_cost_relay(defaults_.costRelay, parse_override(kCostRelayKey));
    }
    CostRelaySettings set_cost_relay(const Json& patch) override {
        std::lock_guard lock(mutex_);
        const auto merged = merge_cost_relay(defaults_.costRelay, spread(parse_override(kCostRelayKey), patch));
        put(kCostRelayKey, json_stringify(to_json(merged)));
        return merged;
    }

    void reload() override {
        std::lock_guard lock(mutex_);
        load_all();
    }

private:
    void load_all() {
        cache_.clear();
        for (const auto& row : db_->all("SELECT key, value FROM runtime_settings", {}, {100'000, 64u << 20}))
            cache_[row.str("key")] = row.str("value");
    }

    /** SQLite から現在時刻を取る (TS と同じく SQL 側で取得)。 */
    std::int64_t now_ms() {
        auto row = db_->get("SELECT CAST(strftime('%s','now') AS INTEGER) * 1000 AS ms");
        return row ? row->i64("ms") : 0;
    }

    void put(const std::string& key, const std::string& value) {
        db_->run(
            "INSERT INTO runtime_settings (key, value, updated_at) VALUES (?, ?, ?)\n"
            "         ON CONFLICT(key) DO UPDATE SET value = excluded.value, updated_at = excluded.updated_at",
            {key, value, now_ms()});
        cache_[key] = value;
    }

    void del(const std::string& key) {
        db_->run("DELETE FROM runtime_settings WHERE key = ?", {key});
        cache_.erase(key);
    }

    void set_text(const std::string& key, const std::optional<std::string>& text) {
        if (!text || str::trim(*text).empty()) del(key);
        else put(key, *text);
    }

    /** 保存済み JSON を読む。無い・空・壊れていれば null。 */
    Json parse_override(const std::string& key) const {
        auto it = cache_.find(key);
        if (it == cache_.end() || it->second.empty()) return nullptr;
        auto parsed = parse_json(it->second, kOverrideLimits);
        return parsed ? *parsed : Json(nullptr);
    }

    std::string lookup(const std::string& key, const std::vector<std::pair<std::string, std::string>>& defs,
                       const std::string& name) const {
        if (auto it = cache_.find(key); it != cache_.end()) return it->second;
        for (const auto& [k, v] : defs)
            if (k == name) return v;
        return "";
    }

    std::vector<OverrideView> views(const std::string& prefix, const std::vector<std::pair<std::string, std::string>>& defs) const {
        std::vector<OverrideView> out;
        for (const auto& [name, def] : defs) {
            OverrideView v;
            v.key = prefix + name;
            v.default_value = def;
            if (auto it = cache_.find(v.key); it != cache_.end()) v.override_value = it->second;
            v.effective = v.override_value ? *v.override_value : def;
            out.push_back(std::move(v));
        }
        return out;
    }

    sql::DatabasePtr db_;
    RuntimeSettingsDefaults defaults_;
    std::map<std::string, std::string> cache_;
    std::mutex mutex_;
};

} // namespace

Json to_json(const FacilitatorTuning& t) {
    return Json{{"tickMs", t.tickMs},
                {"idleGapMs", t.idleGapMs},
                {"maxPersonas", t.maxPersonas},
                {"aufhebungTarget", t.aufhebungTarget},
                {"convergePolicy", t.convergePolicy}};
}

Json to_json(const CostRelaySettings& s) {
    return Json{{"enabled", s.enabled}, {"anatomiaUrl", s.anatomiaUrl}, {"concordiaUrl", s.concordiaUrl}, {"service", s.service}};
}

Json to_json(const OverrideView& v) {
    Json j = Json::object();
    j["key"] = v.key;
    j["default"] = v.default_value;
    j["override"] = v.override_value ? Json(*v.override_value) : Json(nullptr);
    j["effective"] = v.effective;
    return j;
}

FacilitatorTuning merge_facilitator_tuning(const FacilitatorTuning& def, const Json& o) {
    if (!json_truthy(o)) return def;
    FacilitatorTuning t;
    t.tickMs = clamp_int(field(o, "tickMs"), 1000, def.tickMs);
    t.idleGapMs = clamp_int(field(o, "idleGapMs"), 1000, def.idleGapMs);
    t.maxPersonas = clamp_int(field(o, "maxPersonas"), 1, def.maxPersonas);
    t.aufhebungTarget = clamp_int(field(o, "aufhebungTarget"), 1, def.aufhebungTarget);
    t.convergePolicy = normalize_policy(field(o, "convergePolicy"), def.convergePolicy);
    return t;
}

CostRelaySettings merge_cost_relay(const CostRelaySettings& def, const Json& o) {
    if (!json_truthy(o)) return def;
    auto str_or = [&](const char* key, const std::string& fallback) {
        const Json* v = field(o, key);
        return v && v->is_string() ? str::trim(v->get_ref<const std::string&>()) : fallback;
    };
    CostRelaySettings s;
    const Json* enabled = field(o, "enabled");
    s.enabled = enabled && enabled->is_boolean() ? enabled->get<bool>() : def.enabled;
    s.anatomiaUrl = str_or("anatomiaUrl", def.anatomiaUrl);
    s.concordiaUrl = str_or("concordiaUrl", def.concordiaUrl);
    s.service = str_or("service", def.service);
    if (s.service.empty()) s.service = def.service;
    return s;
}

std::shared_ptr<RuntimeSettingsStore> create_runtime_settings_store(sql::DatabasePtr db, RuntimeSettingsDefaults defaults) {
    return std::make_shared<SqliteRuntimeSettingsStore>(std::move(db), std::move(defaults));
}

} // namespace di::runtime_settings
