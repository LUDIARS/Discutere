#include "lib/fundamentum/fundamentum.hpp"

#include <algorithm>
#include <cmath>
#include <set>

#include "base/crypto.hpp"
#include "base/fsutil.hpp"
#include "base/strings.hpp"

namespace di::lib::fundamentum {

namespace {

/** UTF-8 → UTF-16 コード単位 (JS の文字列比較用)。 */
std::u16string to_utf16(std::string_view s) {
    std::u16string out;
    std::size_t i = 0;
    while (i < s.size()) {
        const std::uint32_t cp = str::next_codepoint(s, i);
        if (cp >= 0x10000) {
            const std::uint32_t v = cp - 0x10000;
            out.push_back(static_cast<char16_t>(0xD800 + (v >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + (v & 0x3FF)));
        } else {
            out.push_back(static_cast<char16_t>(cp));
        }
    }
    return out;
}

bool js_less(const std::string& a, const std::string& b) { return to_utf16(a) < to_utf16(b); }

void write(std::string& out, const Json& v) {
    switch (v.type()) {
        case Json::value_t::null:
            out += "null";
            return;
        case Json::value_t::boolean:
            out += v.get<bool>() ? "true" : "false";
            return;
        case Json::value_t::number_integer:
        case Json::value_t::number_unsigned:
        case Json::value_t::number_float: {
            // JS の number は double。JSON.stringify と同じ書式 (-0 は "0")。
            const double d = v.get<double>();
            if (!std::isfinite(d)) throw std::invalid_argument("Fundamentum: 非有限数は canonicalize できない: " + js_number_to_string(d));
            out += json_stringify(Json(d));
            return;
        }
        case Json::value_t::string:
            out += json_stringify(v);
            return;
        case Json::value_t::array: {
            out += '[';
            bool first = true;
            for (const auto& item : v) {
                if (!first) out += ',';
                first = false;
                write(out, item);
            }
            out += ']';
            return;
        }
        case Json::value_t::object: {
            std::vector<std::string> keys;
            keys.reserve(v.size());
            for (auto it = v.begin(); it != v.end(); ++it) keys.push_back(it.key());
            js_sort(keys);
            out += '{';
            bool first = true;
            for (const auto& k : keys) {
                if (!first) out += ',';
                first = false;
                out += json_stringify(Json(k));
                out += ':';
                write(out, v.at(k));
            }
            out += '}';
            return;
        }
        default:
            throw std::invalid_argument("Fundamentum: canonicalize 非対応の型");
    }
}

/** Buffer.from(s, "base64url").toString("utf8") 相当 (Node と同じく寛容: 不正文字は飛ばし、= で止まる)。 */
std::string node_base64url_decode(std::string_view s) {
    std::string out;
    std::uint32_t acc = 0;
    int bits = 0;
    for (char c : s) {
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+' || c == '-') v = 62;
        else if (c == '/' || c == '_') v = 63;
        else if (c == '=') break;
        else continue;
        acc = (acc << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((acc >> bits) & 0xFF));
        }
    }
    return str::sanitize_utf8(out);
}

std::string enc(const std::string& s) { return crypto::base64url_encode(s); }

std::string partition_for(const std::string& scope) { return "user:" + scope; }

const std::string HEAD = "catalog:head";
const std::string VERSION = "catalog:version";

Json parse_stored(const std::string& text) {
    return parse_json_or_throw(text, kStoredJsonLimits);
}

}  // namespace

void js_sort(std::vector<std::string>& values) {
    std::stable_sort(values.begin(), values.end(), js_less);
}

std::string canonicalize(const Json& value) {
    std::string out;
    write(out, value);
    return out;
}

ContentId content_id_from_canonical(std::string_view canonical) {
    return std::string(CONTENT_ID_PREFIX) + crypto::sha256_hex(canonical);
}

ContentId content_id_of(const Json& value) { return content_id_from_canonical(canonicalize(value)); }

bool is_content_id(std::string_view s) {
    if (!str::starts_with(s, CONTENT_ID_PREFIX)) return false;
    const auto hex = s.substr(CONTENT_ID_PREFIX.size());
    if (hex.size() != 64) return false;
    return std::all_of(hex.begin(), hex.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

// ─── JSON ────────────────────────────────────────────────────────

Json to_json(const MasterRecord& r) { return Json{{"id", r.id}, {"value", r.value}}; }

Json to_json(const Catalog& c) {
    Json o = Json::object();
    o["id"] = c.id;
    o["name"] = c.name;
    o["version"] = c.version;
    o["entries"] = c.entries;
    o["parent"] = c.parent ? Json(*c.parent) : Json(nullptr);
    return o;
}

Json to_json(const CatalogDiff& d) {
    return Json{{"added", d.added}, {"removed", d.removed}, {"changed", d.changed}, {"unchanged", d.unchanged}};
}

Json to_json(const UserRecord& r) {
    Json o = Json::object();
    o["scope"] = r.scope;
    o["key"] = r.key;
    o["value"] = r.value;
    o["revision"] = r.revision;
    o["deleted"] = r.deleted;
    return o;
}

Json to_json(const Resolved& r) {
    Json o = Json::object();
    o["key"] = r.key;
    o["value"] = r.value;
    o["source"] = r.source;
    o["id"] = r.id ? Json(*r.id) : Json(nullptr);
    return o;
}

UserRecord user_record_from_json(const Json& j) {
    UserRecord r;
    if (!j.is_object()) return r;
    r.scope = json_string(j, "scope");
    r.key = json_string(j, "key");
    r.value = j.contains("value") ? j.at("value") : Json(nullptr);
    r.revision = json_int(j, "revision");
    r.deleted = json_bool(j, "deleted");
    return r;
}

// ─── MemoryBackend ───────────────────────────────────────────────

std::optional<std::string> MemoryBackend::get_content(const std::string& id) {
    std::lock_guard lock(mutex_);
    auto it = content_.find(id);
    if (it == content_.end()) return std::nullopt;
    return it->second;
}

void MemoryBackend::put_content(const std::string& id, const std::string& canonical) {
    std::lock_guard lock(mutex_);
    content_[id] = canonical;
}

bool MemoryBackend::has_content(const std::string& id) {
    std::lock_guard lock(mutex_);
    return content_.count(id) > 0;
}

std::optional<std::string> MemoryBackend::get_keyed(const std::string& partition, const std::string& key) {
    std::lock_guard lock(mutex_);
    auto p = keyed_.find(partition);
    if (p == keyed_.end()) return std::nullopt;
    auto it = p->second.find(key);
    if (it == p->second.end()) return std::nullopt;
    return it->second;
}

void MemoryBackend::put_keyed(const std::string& partition, const std::string& key, const std::string& json) {
    std::lock_guard lock(mutex_);
    keyed_[partition][key] = json;
}

void MemoryBackend::delete_keyed(const std::string& partition, const std::string& key) {
    std::lock_guard lock(mutex_);
    auto p = keyed_.find(partition);
    if (p != keyed_.end()) p->second.erase(key);
}

std::vector<std::string> MemoryBackend::list_keys(const std::string& partition, const std::string& prefix) {
    std::vector<std::string> keys;
    {
        std::lock_guard lock(mutex_);
        auto p = keyed_.find(partition);
        if (p == keyed_.end()) return keys;
        for (const auto& [k, v] : p->second)
            if (prefix.empty() || str::starts_with(k, prefix)) keys.push_back(k);
    }
    js_sort(keys);
    return keys;
}

// ─── FileBackend ─────────────────────────────────────────────────

FileBackend::FileBackend(std::string dir) : dir_(std::move(dir)) {}

std::optional<std::string> FileBackend::content_path(const std::string& id) const {
    const std::string hex = str::starts_with(id, CONTENT_ID_PREFIX) ? id.substr(CONTENT_ID_PREFIX.size()) : id;
    // C++ 版の追加の防御: ファイル名に使えない (パスを脱出しうる) 文字を含む id は扱わない。
    if (hex.empty() || !std::all_of(hex.begin(), hex.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '-';
        }))
        return std::nullopt;
    const std::string shard = hex.size() >= 2 ? hex.substr(0, 2) : hex;
    return fs::join(fs::join(fs::join(dir_, "content"), shard), hex + ".json");
}

std::optional<std::string> FileBackend::get_content(const std::string& id) {
    auto p = content_path(id);
    if (!p || !fs::is_file(*p)) return std::nullopt;
    return fs::read_file(*p, kMaxStoredBytes);
}

void FileBackend::put_content(const std::string& id, const std::string& canonical) {
    auto p = content_path(id);
    if (!p) throw std::invalid_argument("Fundamentum: invalid content id");
    if (!fs::write_file_atomic(*p, canonical)) throw std::runtime_error("Fundamentum: content write failed");
}

bool FileBackend::has_content(const std::string& id) { return get_content(id).has_value(); }

std::string FileBackend::partition_dir(const std::string& partition) const {
    return fs::join(fs::join(dir_, "keyed"), enc(partition));
}

std::string FileBackend::keyed_path(const std::string& partition, const std::string& key) const {
    return fs::join(partition_dir(partition), enc(key) + ".json");
}

std::optional<std::string> FileBackend::get_keyed(const std::string& partition, const std::string& key) {
    const auto p = keyed_path(partition, key);
    if (!fs::is_file(p)) return std::nullopt;
    return fs::read_file(p, kMaxStoredBytes);
}

void FileBackend::put_keyed(const std::string& partition, const std::string& key, const std::string& json) {
    if (!fs::write_file_atomic(keyed_path(partition, key), json)) throw std::runtime_error("Fundamentum: keyed write failed");
}

void FileBackend::delete_keyed(const std::string& partition, const std::string& key) {
    fs::remove_file(keyed_path(partition, key));  // 不在なら no-op。
}

std::vector<std::string> FileBackend::list_keys(const std::string& partition, const std::string& prefix) {
    std::vector<std::string> keys;
    const auto d = partition_dir(partition);
    if (!fs::is_dir(d)) return keys;
    for (const auto& f : fs::list_dir(d)) {
        if (!str::ends_with(f, ".json")) continue;
        std::string k = node_base64url_decode(std::string_view(f).substr(0, f.size() - 5));
        if (prefix.empty() || str::starts_with(k, prefix)) keys.push_back(std::move(k));
    }
    js_sort(keys);
    return keys;
}

// ─── MasterStore ─────────────────────────────────────────────────

MasterRecord MasterStore::put(const Json& value) {
    const std::string canonical = canonicalize(value);
    const ContentId id = content_id_from_canonical(canonical);
    if (!backend_->has_content(id)) backend_->put_content(id, canonical);
    return {id, value};
}

std::vector<MasterRecord> MasterStore::put_all(const std::vector<Json>& values) {
    std::vector<MasterRecord> out;
    out.reserve(values.size());
    for (const auto& v : values) out.push_back(put(v));
    return out;
}

std::optional<Json> MasterStore::get(const ContentId& id) {
    auto canonical = backend_->get_content(id);
    if (!canonical) return std::nullopt;
    return parse_stored(*canonical);
}

bool MasterStore::has(const ContentId& id) { return backend_->has_content(id); }

// ─── CatalogStore ────────────────────────────────────────────────

std::optional<Catalog> CatalogStore::latest(const std::string& name) {
    auto id = backend_->get_keyed(HEAD, name);
    if (!id) return std::nullopt;
    return load(*id);
}

std::optional<Catalog> CatalogStore::version(const std::string& name, std::int64_t version) {
    auto id = backend_->get_keyed(VERSION, name + "@" + std::to_string(version));
    if (!id) return std::nullopt;
    return load(*id);
}

std::optional<Catalog> CatalogStore::load(const ContentId& id) {
    auto canonical = backend_->get_content(id);
    if (!canonical) return std::nullopt;
    const Json p = parse_stored(*canonical);
    Catalog c;
    c.id = id;
    c.name = json_string(p, "name");
    c.version = json_int(p, "version");
    c.entries = json_object(p, "entries");
    if (auto parent = json_opt_string(p, "parent")) c.parent = *parent;
    return c;
}

std::vector<Catalog> CatalogStore::history(const std::string& name) {
    std::vector<Catalog> out;
    std::set<ContentId> seen;  // 壊れた (循環する) parent で止まらなくなるのを防ぐ
    auto cur = latest(name);
    while (cur && seen.insert(cur->id).second) {
        out.push_back(*cur);
        cur = cur->parent && !cur->parent->empty() ? load(*cur->parent) : std::nullopt;
    }
    return out;
}

std::vector<std::string> CatalogStore::names() { return backend_->list_keys(HEAD); }

CatalogBuilder CatalogStore::builder(const std::string& name) { return CatalogBuilder(*this, name); }

CatalogDiff CatalogStore::diff(const Catalog& a, const Catalog& b) const {
    CatalogDiff d;
    for (auto it = b.entries.begin(); it != b.entries.end(); ++it)
        if (!a.entries.contains(it.key())) d.added.push_back(it.key());
    for (auto it = a.entries.begin(); it != a.entries.end(); ++it) {
        if (!b.entries.contains(it.key())) d.removed.push_back(it.key());
        else if (b.entries.at(it.key()) != it.value()) d.changed.push_back(it.key());
        else d.unchanged.push_back(it.key());
    }
    js_sort(d.added);
    js_sort(d.removed);
    js_sort(d.changed);
    js_sort(d.unchanged);
    return d;
}

Catalog CatalogStore::commit_internal(const std::string& name, const Json& entries, const std::optional<Catalog>& parent) {
    const std::int64_t version = parent ? parent->version + 1 : 1;
    Json payload = Json::object();
    payload["name"] = name;
    payload["version"] = version;
    payload["entries"] = entries;
    payload["parent"] = parent ? Json(parent->id) : Json(nullptr);
    const std::string canonical = canonicalize(payload);
    const ContentId id = content_id_from_canonical(canonical);
    backend_->put_content(id, canonical);
    backend_->put_keyed(VERSION, name + "@" + std::to_string(version), id);
    backend_->put_keyed(HEAD, name, id);
    Catalog c;
    c.id = id;
    c.name = name;
    c.version = version;
    c.entries = entries;
    if (parent) c.parent = parent->id;
    return c;
}

CatalogBuilder& CatalogBuilder::set(const std::string& logical_name, const ContentId& id) {
    ops_.push_back({true, logical_name, id});
    return *this;
}

CatalogBuilder& CatalogBuilder::remove(const std::string& logical_name) {
    ops_.push_back({false, logical_name, {}});
    return *this;
}

Catalog CatalogBuilder::commit() {
    const auto parent = store_.latest(name_);
    Json entries = parent ? parent->entries : Json::object();
    for (const auto& op : ops_) {
        if (op.set) entries[op.name] = op.id;
        else entries.erase(op.name);
    }
    if (parent) {
        bool same = parent->entries.size() == entries.size();
        if (same) {
            for (auto it = parent->entries.begin(); it != parent->entries.end(); ++it) {
                if (!entries.contains(it.key()) || entries.at(it.key()) != it.value()) {
                    same = false;
                    break;
                }
            }
        }
        if (same) return *parent;
    }
    return store_.commit_internal(name_, entries, parent);
}

// ─── UserStore ───────────────────────────────────────────────────

std::vector<UserRecord> UserStore::history(const std::string& scope, const std::string& key) {
    std::vector<UserRecord> out;
    auto raw = backend_->get_keyed(partition_for(scope), key);
    if (!raw) return out;
    const Json list = parse_stored(*raw);
    if (!list.is_array()) throw std::runtime_error("Fundamentum: user history is not an array");
    for (const auto& j : list) out.push_back(user_record_from_json(j));
    return out;
}

namespace {
std::string serialize(const std::vector<UserRecord>& revisions) {
    Json a = Json::array();
    for (const auto& r : revisions) a.push_back(to_json(r));
    return json_stringify(a);
}
}  // namespace

UserRecord UserStore::set(const std::string& scope, const std::string& key, const Json& value) {
    auto revisions = history(scope, key);
    const std::int64_t revision = (revisions.empty() ? 0 : revisions.back().revision) + 1;
    UserRecord record{scope, key, value, revision, false};
    revisions.push_back(record);
    backend_->put_keyed(partition_for(scope), key, serialize(revisions));
    return record;
}

UserRecord UserStore::remove(const std::string& scope, const std::string& key) {
    auto revisions = history(scope, key);
    const std::int64_t revision = (revisions.empty() ? 0 : revisions.back().revision) + 1;
    UserRecord record{scope, key, Json(nullptr), revision, true};
    revisions.push_back(record);
    backend_->put_keyed(partition_for(scope), key, serialize(revisions));
    return record;
}

std::optional<UserRecord> UserStore::get_record(const std::string& scope, const std::string& key) {
    auto revisions = history(scope, key);
    if (revisions.empty()) return std::nullopt;
    return revisions.back();
}

std::optional<Json> UserStore::get(const std::string& scope, const std::string& key) {
    auto record = get_record(scope, key);
    if (!record || record->deleted) return std::nullopt;
    return record->value;
}

std::vector<std::string> UserStore::list(const std::string& scope) {
    std::vector<std::string> live;
    for (const auto& key : backend_->list_keys(partition_for(scope))) {
        auto record = get_record(scope, key);
        if (record && !record->deleted) live.push_back(key);
    }
    return live;
}

// ─── Resolver ────────────────────────────────────────────────────

Resolved Resolver::resolve(const std::string& scope, const std::string& catalog_name, const std::string& name) {
    if (auto user_record = user_.get_record(scope, name)) {
        return {name, user_record->deleted ? Json(nullptr) : user_record->value, "user", std::nullopt};
    }
    const auto catalog = catalog_.latest(catalog_name);
    std::string id;
    if (catalog && catalog->entries.contains(name) && catalog->entries.at(name).is_string())
        id = catalog->entries.at(name).get<std::string>();
    if (id.empty()) return {name, Json(nullptr), "none", std::nullopt};
    auto value = master_.get(id);
    return {name, value ? *value : Json(nullptr), "master", id};
}

std::vector<Resolved> Resolver::resolve_all(const std::string& scope, const std::string& catalog_name) {
    const auto catalog = catalog_.latest(catalog_name);
    std::set<std::string> seen;
    std::vector<std::string> names;
    if (catalog)
        for (auto it = catalog->entries.begin(); it != catalog->entries.end(); ++it)
            if (seen.insert(it.key()).second) names.push_back(it.key());
    for (const auto& key : user_.list(scope))
        if (seen.insert(key).second) names.push_back(key);
    js_sort(names);
    std::vector<Resolved> out;
    for (const auto& n : names) out.push_back(resolve(scope, catalog_name, n));
    return out;
}

// ─── Foundation ──────────────────────────────────────────────────

Foundation::Foundation(std::shared_ptr<StorageBackend> b)
    : backend(std::move(b)), master(backend), catalog(backend), user(backend), resolver(master, catalog, user) {}

std::unique_ptr<Foundation> Foundation::in_memory() { return std::make_unique<Foundation>(std::make_shared<MemoryBackend>()); }

std::unique_ptr<Foundation> Foundation::on_disk(const std::string& dir) {
    return std::make_unique<Foundation>(std::make_shared<FileBackend>(dir));
}

} // namespace di::lib::fundamentum
