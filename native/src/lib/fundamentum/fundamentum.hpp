#pragma once
// fundamentum (lib/fundamentum/src) の移植 — LUDIARS 共通データ基盤。
// master (content-addressed 不変) / user (可変 overlay) の 2 層と、論理名付けの catalog、
// 実効ビューの resolver を提供する。TS の async API は同期にした (呼んだスレッドで I/O する)。
//
// 値は「不透明な JSON 値」(di::Json) として扱う。content id は `fm1:` + sha256(canonical JSON)。
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "base/json.hpp"

namespace di::lib::fundamentum {

using ContentId = std::string;

/** content id の prefix。アルゴリズム/版を将来切り替えられるよう明示する。 */
inline constexpr std::string_view CONTENT_ID_PREFIX = "fm1:";

/** 保存済みの値を読み戻すときの上限 (ファイル・メモリの中身も外部入力として課金する)。 */
inline constexpr std::size_t kMaxStoredBytes = 64u << 20;
inline const JsonLimits kStoredJsonLimits{kMaxStoredBytes, 4'000'000, 128};

// ─── hash ────────────────────────────────────────────────────────

/**
 * 決定的 canonical JSON 直列化。オブジェクトのキーを (JS の sort と同じ UTF-16 順で) 再帰的にソートする。
 * 非有限数 (NaN / Infinity) は std::invalid_argument。数値は JS の JSON.stringify と同じ書式。
 */
std::string canonicalize(const Json& value);
/** 既に canonical 化された文字列から content id を作る。 */
ContentId content_id_from_canonical(std::string_view canonical);
/** JSON 値から content id を作る。 */
ContentId content_id_of(const Json& value);
/** 文字列が本基盤の content id 形式かを判定する (sha256 hex = 64 桁)。 */
bool is_content_id(std::string_view s);
/** JS の Array#sort (既定比較 = UTF-16 コード単位順) と同じ並べ替え。 */
void js_sort(std::vector<std::string>& values);

// ─── 型 ──────────────────────────────────────────────────────────

struct MasterRecord {
    ContentId id;
    Json value;
};

/** 名前付き・versioned スナップショット (entries は 論理名 → master content id の object)。 */
struct Catalog {
    ContentId id;
    std::string name;
    std::int64_t version = 0;
    Json entries = Json::object();
    std::optional<ContentId> parent;
};

struct CatalogDiff {
    std::vector<std::string> added;
    std::vector<std::string> removed;
    std::vector<std::string> changed;
    std::vector<std::string> unchanged;
};

/** user 層の 1 リビジョン (履歴の 1 要素)。 */
struct UserRecord {
    std::string scope;
    std::string key;
    /** tombstone のときは null。 */
    Json value;
    std::int64_t revision = 0;
    bool deleted = false;
};

/** master + user overlay を統合した実効値。source は "user" | "master" | "none"。 */
struct Resolved {
    std::string key;
    Json value;
    std::string source;
    std::optional<ContentId> id;
};

Json to_json(const MasterRecord& r);
Json to_json(const Catalog& c);
Json to_json(const CatalogDiff& d);
Json to_json(const UserRecord& r);
Json to_json(const Resolved& r);
UserRecord user_record_from_json(const Json& j);

// ─── backend ─────────────────────────────────────────────────────

/**
 * 永続層の抽象。content (不変・content-addressed) と keyed (可変 key-value) の 2 系統。
 * 値は不透明な文字列として扱う (直列化/解釈は上位)。
 */
class StorageBackend {
public:
    virtual ~StorageBackend() = default;
    virtual std::optional<std::string> get_content(const std::string& id) = 0;
    virtual void put_content(const std::string& id, const std::string& canonical) = 0;
    virtual bool has_content(const std::string& id) = 0;
    virtual std::optional<std::string> get_keyed(const std::string& partition, const std::string& key) = 0;
    virtual void put_keyed(const std::string& partition, const std::string& key, const std::string& json) = 0;
    virtual void delete_keyed(const std::string& partition, const std::string& key) = 0;
    /** partition 内の key 一覧 (JS の sort 順)。prefix 指定でフィルタ。partition 不在なら []。 */
    virtual std::vector<std::string> list_keys(const std::string& partition, const std::string& prefix = {}) = 0;
};

/** in-memory (hermetic) な backend。複数スレッドから使ってよい。 */
class MemoryBackend : public StorageBackend {
public:
    std::optional<std::string> get_content(const std::string& id) override;
    void put_content(const std::string& id, const std::string& canonical) override;
    bool has_content(const std::string& id) override;
    std::optional<std::string> get_keyed(const std::string& partition, const std::string& key) override;
    void put_keyed(const std::string& partition, const std::string& key, const std::string& json) override;
    void delete_keyed(const std::string& partition, const std::string& key) override;
    std::vector<std::string> list_keys(const std::string& partition, const std::string& prefix = {}) override;

private:
    std::mutex mutex_;
    std::map<std::string, std::string> content_;
    std::map<std::string, std::map<std::string, std::string>> keyed_;
};

/**
 * ディスク永続な backend。
 *  - content: `content/<aa>/<hash>.json` (aa = hash 先頭 2 桁で sharding)
 *  - keyed:   `keyed/<enc(partition)>/<enc(key)>.json` (enc = base64url、fs 安全)
 * 書込みは tmp + rename で atomic。壊れた/読めないエントリは miss 扱い。
 * (C++ 版の追加の防御) content id の hash 部が [0-9A-Za-z_-] 以外を含む場合は読まない (パスの脱出を防ぐ)。
 */
class FileBackend : public StorageBackend {
public:
    explicit FileBackend(std::string dir);
    std::optional<std::string> get_content(const std::string& id) override;
    void put_content(const std::string& id, const std::string& canonical) override;
    bool has_content(const std::string& id) override;
    std::optional<std::string> get_keyed(const std::string& partition, const std::string& key) override;
    void put_keyed(const std::string& partition, const std::string& key, const std::string& json) override;
    void delete_keyed(const std::string& partition, const std::string& key) override;
    std::vector<std::string> list_keys(const std::string& partition, const std::string& prefix = {}) override;
    const std::string& dir() const { return dir_; }

private:
    std::optional<std::string> content_path(const std::string& id) const;
    std::string partition_dir(const std::string& partition) const;
    std::string keyed_path(const std::string& partition, const std::string& key) const;
    std::string dir_;
};

// ─── stores ──────────────────────────────────────────────────────

/** content-addressed・不変ストア。update / delete は持たない。 */
class MasterStore {
public:
    explicit MasterStore(std::shared_ptr<StorageBackend> backend) : backend_(std::move(backend)) {}
    /** 値を put し MasterRecord を返す。既存 (同 id) ならば書込みを省く (冪等)。 */
    MasterRecord put(const Json& value);
    std::vector<MasterRecord> put_all(const std::vector<Json>& values);
    /** content id から値を取得。無ければ nullopt。 */
    std::optional<Json> get(const ContentId& id);
    bool has(const ContentId& id);

private:
    std::shared_ptr<StorageBackend> backend_;
};

class CatalogBuilder;

/** 名前付き・versioned スナップショット (論理名 → master content id) の版管理。 */
class CatalogStore {
public:
    explicit CatalogStore(std::shared_ptr<StorageBackend> backend) : backend_(std::move(backend)) {}
    std::optional<Catalog> latest(const std::string& name);
    std::optional<Catalog> version(const std::string& name, std::int64_t version);
    std::optional<Catalog> load(const ContentId& id);
    /** latest → parent と遡った全版 (latest 先頭)。 */
    std::vector<Catalog> history(const std::string& name);
    std::vector<std::string> names();
    CatalogBuilder builder(const std::string& name);
    /** 2 版の差分。a → b で何が変わったか。 */
    CatalogDiff diff(const Catalog& a, const Catalog& b) const;
    /** builder からのみ呼ばれる内部 commit。 */
    Catalog commit_internal(const std::string& name, const Json& entries, const std::optional<Catalog>& parent);

private:
    std::shared_ptr<StorageBackend> backend_;
};

/** Catalog の更新ビルダー。操作を積み、commit 時に latest をベースに適用する。 */
class CatalogBuilder {
public:
    CatalogBuilder(CatalogStore& store, std::string name) : store_(store), name_(std::move(name)) {}
    CatalogBuilder& set(const std::string& logical_name, const ContentId& id);
    CatalogBuilder& remove(const std::string& logical_name);
    /** 結果が現 latest と同一エントリなら新版を作らず latest をそのまま返す (no-op 更新)。 */
    Catalog commit();

private:
    struct Op {
        bool set;
        std::string name;
        ContentId id;
    };
    CatalogStore& store_;
    std::string name_;
    std::vector<Op> ops_;
};

/** scoped・可変・revisioned overlay。delete は tombstone リビジョン。 */
class UserStore {
public:
    explicit UserStore(std::shared_ptr<StorageBackend> backend) : backend_(std::move(backend)) {}
    UserRecord set(const std::string& scope, const std::string& key, const Json& value);
    UserRecord remove(const std::string& scope, const std::string& key);  // TS の delete
    /** key の最新の live な値。未設定・最新が tombstone なら nullopt。 */
    std::optional<Json> get(const std::string& scope, const std::string& key);
    std::optional<UserRecord> get_record(const std::string& scope, const std::string& key);
    std::vector<UserRecord> history(const std::string& scope, const std::string& key);
    /** scope 内の live な key 一覧。 */
    std::vector<std::string> list(const std::string& scope);

private:
    std::shared_ptr<StorageBackend> backend_;
};

/** master + user overlay の実効ビュー。 */
class Resolver {
public:
    Resolver(MasterStore& master, CatalogStore& catalog, UserStore& user) : master_(master), catalog_(catalog), user_(user) {}
    Resolved resolve(const std::string& scope, const std::string& catalog_name, const std::string& name);
    std::vector<Resolved> resolve_all(const std::string& scope, const std::string& catalog_name);

private:
    MasterStore& master_;
    CatalogStore& catalog_;
    UserStore& user_;
};

/** facade。backend を渡すと master / catalog / user / resolver を配線する。 */
class Foundation {
public:
    explicit Foundation(std::shared_ptr<StorageBackend> backend);
    static std::unique_ptr<Foundation> in_memory();
    static std::unique_ptr<Foundation> on_disk(const std::string& dir);
    Resolved resolve(const std::string& scope, const std::string& catalog_name, const std::string& name) {
        return resolver.resolve(scope, catalog_name, name);
    }
    std::vector<Resolved> resolve_all(const std::string& scope, const std::string& catalog_name) {
        return resolver.resolve_all(scope, catalog_name);
    }

    std::shared_ptr<StorageBackend> backend;
    MasterStore master;
    CatalogStore catalog;
    UserStore user;
    Resolver resolver;
};

} // namespace di::lib::fundamentum
