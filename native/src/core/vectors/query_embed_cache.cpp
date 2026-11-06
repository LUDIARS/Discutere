#include "base/sort_by_index.hpp"
#include "core/vectors/query_embed_cache.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>

#include "base/crypto.hpp"
#include "base/executor.hpp"
#include "base/json.hpp"
#include "base/strings.hpp"
#include "base/time.hpp"
#include "core/vectors/vector_search.hpp"
#include "visualize/locale_compare.hpp"

namespace di::core::vectors {

namespace {

struct State {
    std::mutex mutex;
    std::map<std::string, std::vector<double>> memory;
    std::map<std::string, std::shared_future<void>> inflight;
    /** 接続ごとの CREATE / 旧 query_text 消去を一度だけにする (接続が閉じられたら無効)。 */
    std::map<const sql::Database*, std::weak_ptr<sql::Database>> initialized;
    /** 同じ on-disk DB を開き直すたびに旧本文消去の UPDATE を繰り返さない。 */
    std::set<std::string> sanitized_paths;
};

State& state() {
    static State s;
    return s;
}

bool is_finite_vector(const std::vector<double>& v) {
    if (v.empty()) return false;
    return std::all_of(v.begin(), v.end(), [](double x) { return std::isfinite(x); });
}

void remember(const std::string& key, const std::vector<double>& vec) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (s.memory.size() >= kQueryCacheMaxMemoryEntries) s.memory.clear();
    s.memory[key] = vec;
}

void persist_query_vector(const sql::DatabasePtr& raw, const std::string& database_path, const std::string& key,
                          const std::vector<double>& vec) {
    const bool can_reopen = database_path != ":memory:" && !database_path.empty();
    sql::DatabasePtr db = raw && raw->is_open() ? raw : (can_reopen ? sql::Database::open(database_path) : nullptr);
    if (!db) return;
    const bool owns = db != raw;
    try {
        ensure_query_cache_table(*db);
        Json arr = Json::array();
        for (double x : vec) arr.push_back(x);
        db->run(R"SQL(INSERT INTO embedding_query_cache (query_hash, query_text, vector_json, created_at)
         VALUES (?, '', ?, ?)
         ON CONFLICT(query_hash) DO UPDATE SET
           query_text = '', vector_json = excluded.vector_json, created_at = excluded.created_at)SQL",
                {key, json_stringify(arr), static_cast<double>(time::now_ms())});
        const auto row = db->get("SELECT COUNT(*) AS n FROM embedding_query_cache");
        const std::int64_t count = row ? row->i64("n") : 0;
        if (count > static_cast<std::int64_t>(kQueryCacheMaxRows)) {
            db->run(R"SQL(DELETE FROM embedding_query_cache WHERE query_hash IN (
             SELECT query_hash FROM embedding_query_cache ORDER BY created_at ASC LIMIT ?
           ))SQL",
                    {count - static_cast<std::int64_t>(kQueryCacheMaxRows)});
        }
    } catch (...) {
        if (owns) db->close();
        throw;
    }
    if (owns) db->close();
}

} // namespace

std::string hash_query(const std::string& model, const std::string& text) { return crypto::sha256_hex(model + "\n" + text); }

void ensure_query_cache_table(sql::Database& raw) {
    auto& s = state();
    {
        std::lock_guard lock(s.mutex);
        auto it = s.initialized.find(&raw);
        if (it != s.initialized.end()) {
            auto alive = it->second.lock();
            if (alive && alive.get() == &raw && raw.is_open()) return;
        }
    }
    raw.run(R"SQL(CREATE TABLE IF NOT EXISTS embedding_query_cache (
         query_hash TEXT PRIMARY KEY,
         query_text TEXT NOT NULL DEFAULT '',
         vector_json TEXT NOT NULL,
         created_at INTEGER NOT NULL
       ))SQL");
    const std::string name = raw.path();
    bool sanitize = false;
    {
        std::lock_guard lock(s.mutex);
        sanitize = name == ":memory:" || !s.sanitized_paths.count(name);
    }
    if (sanitize) {
        // 初期実装で保存した query_text も、キャッシュ利用時にデータ最小化する。
        raw.run("UPDATE embedding_query_cache SET query_text = '' WHERE query_text <> ''");
    }
    std::lock_guard lock(s.mutex);
    if (sanitize && name != ":memory:") s.sanitized_paths.insert(name);
    std::weak_ptr<sql::Database> weak;
    try {
        weak = raw.shared_from_this();
    } catch (const std::bad_weak_ptr&) {
    }
    s.initialized[&raw] = weak;
}

std::string normalize_query_text(const std::vector<std::string>& terms) {
    std::vector<std::string> out;
    for (const auto& t : terms) {
        std::string v = str::to_lower(str::trim(t));
        if (v.empty()) continue;
        if (std::find(out.begin(), out.end(), v) == out.end()) out.push_back(std::move(v));
    }
    // Array#sort の既定 (UTF-16 のコード単位順)。
    di::stable_sort_by_index(out, [](const std::string& a, const std::string& b) { return visualize::js_default_compare(a, b) < 0; });
    return str::join(out, " ");
}

std::optional<std::vector<double>> get_cached_query_vector(sql::Database& raw, const std::string& model, const std::string& query_text) {
    const std::string key = hash_query(model, query_text);
    {
        auto& s = state();
        std::lock_guard lock(s.mutex);
        auto it = s.memory.find(key);
        if (it != s.memory.end()) return it->second;
    }
    ensure_query_cache_table(raw);
    const auto row = raw.get("SELECT vector_json FROM embedding_query_cache WHERE query_hash = ?", {key});
    if (!row) return std::nullopt;
    auto vec = parse_vector_json(row->str("vector_json"));
    if (!vec) return std::nullopt;
    remember(key, *vec);
    return vec;
}

namespace {

void do_warm(const sql::DatabasePtr& raw, const std::string& database_path, const std::shared_ptr<EmbeddingClient>& embedder,
             const std::string& key, const std::string& query_text, const WarnFn& warn) {
    try {
        if (!embedder) throw std::runtime_error("埋め込みクライアントが無い");
        const auto vectors = embedder->embed({query_text});
        if (vectors.empty() || !is_finite_vector(vectors.front())) throw std::runtime_error("埋め込み応答が有効な有限数ベクトルではない");
        remember(key, vectors.front());
        persist_query_vector(raw, database_path, key, vectors.front());
    } catch (const std::exception& e) {
        if (warn) warn(std::string("クエリ埋め込みの温めに失敗 (キーワード検索で継続): ") + e.what());
    }
}

/** 同じキーの仕事が走っていればその future、無ければ新しい promise を登録して返す。 */
std::pair<std::shared_future<void>, std::shared_ptr<std::promise<void>>> claim(const std::string& key) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (s.memory.count(key)) {
        std::promise<void> done;
        done.set_value();
        return {done.get_future().share(), nullptr};
    }
    auto it = s.inflight.find(key);
    if (it != s.inflight.end()) return {it->second, nullptr};
    auto p = std::make_shared<std::promise<void>>();
    auto f = p->get_future().share();
    s.inflight[key] = f;
    return {f, p};
}

void release(const std::string& key, const std::shared_ptr<std::promise<void>>& p) {
    {
        auto& s = state();
        std::lock_guard lock(s.mutex);
        s.inflight.erase(key);
    }
    p->set_value();
}

} // namespace

void warm_query_embedding_sync(const sql::DatabasePtr& raw, const std::shared_ptr<EmbeddingClient>& embedder, const std::string& model,
                               const std::string& query_text, const WarnFn& warn) {
    const std::string key = hash_query(model, query_text);
    const std::string path = raw ? raw->path() : std::string();
    auto [future, owner] = claim(key);
    if (!owner) {
        future.wait();
        return;
    }
    do_warm(raw, path, embedder, key, query_text, warn);
    release(key, owner);
}

std::shared_future<void> warm_query_embedding(const sql::DatabasePtr& raw, const std::shared_ptr<EmbeddingClient>& embedder,
                                              const std::string& model, const std::string& query_text, const WarnFn& warn) {
    const std::string key = hash_query(model, query_text);
    const std::string path = raw ? raw->path() : std::string();
    auto [future, owner] = claim(key);
    if (!owner) return future;
    const bool queued = background_executor().submit([raw, path, embedder, key, query_text, warn, owner = owner] {
        do_warm(raw, path, embedder, key, query_text, warn);
        release(key, owner);
    });
    if (!queued) {
        if (warn) warn("クエリ埋め込みの温めに失敗 (キーワード検索で継続): background queue is full");
        release(key, owner);
    }
    return future;
}

void reset_query_embed_cache_for_test() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.memory.clear();
    s.inflight.clear();
}

} // namespace di::core::vectors
