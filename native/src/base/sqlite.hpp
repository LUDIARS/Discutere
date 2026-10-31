#pragma once
// SQLite (better-sqlite3 と同じ amalgamation) の薄いラッパ。使い方は better-sqlite3 に寄せる:
//   db->run(sql, {a, b})   … stmt.run(a, b)   → {changes, lastInsertRowid}
//   db->get(sql, {a})      … stmt.get(a)      → 1 行 (無ければ nullopt)
//   db->all(sql, {a})      … stmt.all(a)      → 全行
//   db->exec(sql)          … db.exec(sql)     (複数文)
//   db->transaction([&]{ ... })  … db.transaction(fn)()
// 名前つき引数は sql::Params::named({{"id", v}}) で渡す (SQL 側は @id / :id / $id)。
// 1 つの接続は内部の recursive_mutex で直列化するので、複数スレッドから使ってよい。
// DB から読む値も外部入力として扱い、all() は行数・バイト数の上限 (lorica::input_budget) で課金する。
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

#include "base/json.hpp"

struct sqlite3;
struct sqlite3_stmt;

namespace di::sql {

struct Blob {
    std::string bytes;
    bool operator==(const Blob&) const = default;
};

/** SQLite の値 (NULL / INTEGER / REAL / TEXT / BLOB)。 */
class Value {
public:
    using Storage = std::variant<std::monostate, std::int64_t, double, std::string, Blob>;
    Value() = default;
    Value(std::nullptr_t) {}
    Value(std::nullopt_t) {}
    Value(int v) : v_(static_cast<std::int64_t>(v)) {}
    Value(unsigned v) : v_(static_cast<std::int64_t>(v)) {}
    Value(long v) : v_(static_cast<std::int64_t>(v)) {}
    Value(long long v) : v_(static_cast<std::int64_t>(v)) {}
    Value(unsigned long v) : v_(static_cast<std::int64_t>(v)) {}
    Value(unsigned long long v) : v_(static_cast<std::int64_t>(v)) {}
    Value(bool v) : v_(static_cast<std::int64_t>(v ? 1 : 0)) {}
    Value(double v) : v_(v) {}
    Value(float v) : v_(static_cast<double>(v)) {}
    Value(const char* v) : v_(v ? Storage(std::string(v)) : Storage()) {}
    Value(std::string v) : v_(std::move(v)) {}
    Value(std::string_view v) : v_(std::string(v)) {}
    Value(Blob v) : v_(std::move(v)) {}
    template <class T>
    Value(const std::optional<T>& v) {
        if (v) *this = Value(*v);
    }

    bool is_null() const { return std::holds_alternative<std::monostate>(v_); }
    bool is_int() const { return std::holds_alternative<std::int64_t>(v_); }
    bool is_real() const { return std::holds_alternative<double>(v_); }
    bool is_text() const { return std::holds_alternative<std::string>(v_); }
    bool is_blob() const { return std::holds_alternative<Blob>(v_); }
    bool is_number() const { return is_int() || is_real(); }

    /** TEXT ならそれ、数値なら JS の String(n) と同じ文字列、NULL は fallback。 */
    std::string as_string(std::string_view fallback = {}) const;
    std::optional<std::string> as_opt_string() const;
    std::int64_t as_int(std::int64_t fallback = 0) const;
    double as_double(double fallback = 0) const;
    const Storage& storage() const { return v_; }
    /** better-sqlite3 が返す JS 値と同じ形の JSON (INTEGER→整数, REAL→数, BLOB→null)。 */
    Json to_json() const;
    bool operator==(const Value& other) const { return v_ == other.v_; }

private:
    Storage v_;
};

/** 1 行。列名で引ける (better-sqlite3 の行オブジェクト相当)。 */
class Row {
public:
    void push(std::string name, Value value);
    bool has(std::string_view column) const;
    /** 無い列は NULL を返す。 */
    const Value& operator[](std::string_view column) const;
    std::string str(std::string_view column, std::string_view fallback = {}) const;
    std::optional<std::string> opt_str(std::string_view column) const;
    std::int64_t i64(std::string_view column, std::int64_t fallback = 0) const;
    std::optional<std::int64_t> opt_i64(std::string_view column) const;
    double num(std::string_view column, double fallback = 0) const;
    std::optional<double> opt_num(std::string_view column) const;
    bool is_null(std::string_view column) const;
    std::size_t size() const { return names_.size(); }
    const std::string& name(std::size_t i) const { return names_[i]; }
    const Value& value(std::size_t i) const { return values_[i]; }
    /** 列名 → 値の object (列の順序を保つ)。 */
    Json to_json() const;

private:
    std::vector<std::string> names_;
    std::vector<Value> values_;
};

/** 位置引数 ({a, b}) か名前つき引数 (named({{"k", v}}))。 */
class Params {
public:
    Params() = default;
    Params(std::initializer_list<Value> values) : positional_(values) {}
    Params(std::vector<Value> values) : positional_(std::move(values)) {}
    static Params named(std::vector<std::pair<std::string, Value>> values);
    void add(Value v) { positional_.push_back(std::move(v)); }
    void set(std::string name, Value v) { named_.emplace_back(std::move(name), std::move(v)); }
    const std::vector<Value>& positional() const { return positional_; }
    const std::vector<std::pair<std::string, Value>>& named_values() const { return named_; }

private:
    std::vector<Value> positional_;
    std::vector<std::pair<std::string, Value>> named_;
};

struct RunResult {
    std::int64_t changes = 0;
    std::int64_t last_insert_rowid = 0;
};

class Error : public std::runtime_error {
public:
    Error(std::string message, int code, int extended_code)
        : std::runtime_error(std::move(message)), code_(code), extended_code_(extended_code) {}
    int code() const { return code_; }
    int extended_code() const { return extended_code_; }
    /** better-sqlite3 の err.code と同じ名前 (例 "SQLITE_CONSTRAINT_UNIQUE")。 */
    std::string code_name() const;
    bool is_constraint() const;
    bool is_busy() const;

private:
    int code_;
    int extended_code_;
};

struct QueryLimits {
    std::size_t max_rows = 2'000'000;
    std::size_t max_bytes = 512u << 20;
};

struct OpenOptions {
    bool readonly = false;
    bool create = true;          // 無ければ作る (better-sqlite3 の既定と同じ)
    bool fileMustExist = false;  // better-sqlite3 の fileMustExist
    int busy_timeout_ms = 5000;  // better-sqlite3 の既定 timeout と同じ
};

class Database : public std::enable_shared_from_this<Database> {
public:
    /** 開く (親ディレクトリは作らない。必要なら呼び出し側で fs::ensure_dir)。失敗は sql::Error。 */
    static std::shared_ptr<Database> open(const std::string& path, const OpenOptions& options = {});
    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    void exec(std::string_view sql);
    RunResult run(std::string_view sql, const Params& params = {});
    std::optional<Row> get(std::string_view sql, const Params& params = {});
    std::vector<Row> all(std::string_view sql, const Params& params = {}, const QueryLimits& limits = {});
    /** 1 行ずつ渡す (大きな結果を溜めずに処理する)。fn が false を返したら止める。 */
    void each(std::string_view sql, const Params& params, const std::function<bool(const Row&)>& fn);
    /** db.pragma("x") の単一値版 (例 pragma_value("journal_mode"))。 */
    std::optional<Value> pragma_value(std::string_view pragma);
    /** db.pragma("journal_mode = WAL") 相当 (結果は捨てる)。 */
    void pragma(std::string_view statement);

    /**
     * better-sqlite3 の db.transaction(fn)() と同じ: 外側なら BEGIN … COMMIT、入れ子なら SAVEPOINT。
     * fn が例外を投げたら ROLLBACK して投げ直す。
     */
    template <class F>
    auto transaction(F&& fn) -> decltype(fn()) {
        std::lock_guard lock(mutex_);
        const bool outer = depth_ == 0;
        const std::string sp = "di_sp_" + std::to_string(depth_);
        exec(outer ? "BEGIN" : "SAVEPOINT " + sp);
        ++depth_;
        try {
            if constexpr (std::is_void_v<decltype(fn())>) {
                fn();
                --depth_;
                exec(outer ? "COMMIT" : "RELEASE " + sp);
            } else {
                auto r = fn();
                --depth_;
                exec(outer ? "COMMIT" : "RELEASE " + sp);
                return r;
            }
        } catch (...) {
            --depth_;
            try {
                if (outer) exec("ROLLBACK");
                else {
                    exec("ROLLBACK TO " + sp);
                    exec("RELEASE " + sp);
                }
            } catch (...) {
            }
            throw;
        }
    }

    bool in_transaction() const;
    std::int64_t changes() const;
    const std::string& path() const { return path_; }
    bool is_open() const;
    void close();
    /** 生のハンドル (使う間は mutex() を握ること)。 */
    sqlite3* handle() { return db_; }
    std::recursive_mutex& mutex() { return mutex_; }
    /** テーブルが存在するか。 */
    bool table_exists(std::string_view table);
    /** テーブルの列名一覧 (PRAGMA table_info)。 */
    std::vector<std::string> table_columns(std::string_view table);

private:
    Database() = default;
    sqlite3_stmt* prepare_cached(std::string_view sql);
    void bind(sqlite3_stmt* stmt, const Params& params);
    [[noreturn]] void throw_error(std::string_view context);
    sqlite3* db_ = nullptr;
    std::string path_;
    mutable std::recursive_mutex mutex_;
    std::unordered_map<std::string, sqlite3_stmt*> cache_;
    int depth_ = 0;
};

using DatabasePtr = std::shared_ptr<Database>;

} // namespace di::sql
