#include "base/sqlite.hpp"

#include <sqlite3.h>

#include <lorica/input_budget.hpp>

#include "base/json.hpp"

namespace di::sql {

// ─── Value ───────────────────────────────────────────────────────

std::string Value::as_string(std::string_view fallback) const {
    if (const auto* s = std::get_if<std::string>(&v_)) return *s;
    if (const auto* i = std::get_if<std::int64_t>(&v_)) return std::to_string(*i);
    if (const auto* d = std::get_if<double>(&v_)) return js_number_to_string(*d);
    if (const auto* b = std::get_if<Blob>(&v_)) return b->bytes;
    return std::string(fallback);
}

std::optional<std::string> Value::as_opt_string() const {
    if (is_null()) return std::nullopt;
    return as_string();
}

std::int64_t Value::as_int(std::int64_t fallback) const {
    if (const auto* i = std::get_if<std::int64_t>(&v_)) return *i;
    if (const auto* d = std::get_if<double>(&v_)) return static_cast<std::int64_t>(*d);
    if (const auto* s = std::get_if<std::string>(&v_)) {
        try {
            return std::stoll(*s);
        } catch (...) {
            return fallback;
        }
    }
    return fallback;
}

double Value::as_double(double fallback) const {
    if (const auto* i = std::get_if<std::int64_t>(&v_)) return static_cast<double>(*i);
    if (const auto* d = std::get_if<double>(&v_)) return *d;
    if (const auto* s = std::get_if<std::string>(&v_)) {
        try {
            return std::stod(*s);
        } catch (...) {
            return fallback;
        }
    }
    return fallback;
}

Json Value::to_json() const {
    if (const auto* i = std::get_if<std::int64_t>(&v_)) return *i;
    if (const auto* d = std::get_if<double>(&v_)) return *d;
    if (const auto* s = std::get_if<std::string>(&v_)) return *s;
    return nullptr;
}

// ─── Row ─────────────────────────────────────────────────────────

void Row::push(std::string name, Value value) {
    // better-sqlite3 と同じく、同名の列は後のものが勝つ。
    for (std::size_t i = 0; i < names_.size(); ++i) {
        if (names_[i] == name) {
            values_[i] = std::move(value);
            return;
        }
    }
    names_.push_back(std::move(name));
    values_.push_back(std::move(value));
}

bool Row::has(std::string_view column) const {
    for (const auto& n : names_)
        if (n == column) return true;
    return false;
}

const Value& Row::operator[](std::string_view column) const {
    static const Value null_value;
    for (std::size_t i = 0; i < names_.size(); ++i)
        if (names_[i] == column) return values_[i];
    return null_value;
}

std::string Row::str(std::string_view column, std::string_view fallback) const { return (*this)[column].as_string(fallback); }
std::optional<std::string> Row::opt_str(std::string_view column) const { return (*this)[column].as_opt_string(); }
std::int64_t Row::i64(std::string_view column, std::int64_t fallback) const { return (*this)[column].as_int(fallback); }
std::optional<std::int64_t> Row::opt_i64(std::string_view column) const {
    const auto& v = (*this)[column];
    if (v.is_null()) return std::nullopt;
    return v.as_int();
}
double Row::num(std::string_view column, double fallback) const { return (*this)[column].as_double(fallback); }
std::optional<double> Row::opt_num(std::string_view column) const {
    const auto& v = (*this)[column];
    if (v.is_null()) return std::nullopt;
    return v.as_double();
}
bool Row::is_null(std::string_view column) const { return (*this)[column].is_null(); }

Json Row::to_json() const {
    Json out = Json::object();
    for (std::size_t i = 0; i < names_.size(); ++i) out[names_[i]] = values_[i].to_json();
    return out;
}

Params Params::named(std::vector<std::pair<std::string, Value>> values) {
    Params p;
    p.named_ = std::move(values);
    return p;
}

// ─── Error ───────────────────────────────────────────────────────

std::string Error::code_name() const {
    switch (extended_code_) {
    case SQLITE_CONSTRAINT_UNIQUE: return "SQLITE_CONSTRAINT_UNIQUE";
    case SQLITE_CONSTRAINT_PRIMARYKEY: return "SQLITE_CONSTRAINT_PRIMARYKEY";
    case SQLITE_CONSTRAINT_FOREIGNKEY: return "SQLITE_CONSTRAINT_FOREIGNKEY";
    case SQLITE_CONSTRAINT_NOTNULL: return "SQLITE_CONSTRAINT_NOTNULL";
    case SQLITE_CONSTRAINT_CHECK: return "SQLITE_CONSTRAINT_CHECK";
    case SQLITE_BUSY_SNAPSHOT: return "SQLITE_BUSY_SNAPSHOT";
    default: break;
    }
    switch (code_) {
    case SQLITE_ERROR: return "SQLITE_ERROR";
    case SQLITE_BUSY: return "SQLITE_BUSY";
    case SQLITE_LOCKED: return "SQLITE_LOCKED";
    case SQLITE_READONLY: return "SQLITE_READONLY";
    case SQLITE_CONSTRAINT: return "SQLITE_CONSTRAINT";
    case SQLITE_CORRUPT: return "SQLITE_CORRUPT";
    case SQLITE_FULL: return "SQLITE_FULL";
    case SQLITE_CANTOPEN: return "SQLITE_CANTOPEN";
    case SQLITE_MISMATCH: return "SQLITE_MISMATCH";
    case SQLITE_TOOBIG: return "SQLITE_TOOBIG";
    case SQLITE_RANGE: return "SQLITE_RANGE";
    default: return "SQLITE_ERROR";
    }
}

bool Error::is_constraint() const { return code_ == SQLITE_CONSTRAINT; }
bool Error::is_busy() const { return code_ == SQLITE_BUSY || code_ == SQLITE_LOCKED; }

// ─── Database ────────────────────────────────────────────────────

std::shared_ptr<Database> Database::open(const std::string& path, const OpenOptions& options) {
    std::shared_ptr<Database> db(new Database());
    db->path_ = path;
    int flags = options.readonly ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE;
    if (!options.readonly && options.create && !options.fileMustExist) flags |= SQLITE_OPEN_CREATE;
    flags |= SQLITE_OPEN_NOMUTEX;  // 直列化はこのラッパの mutex で行う (SQLITE_THREADSAFE=2)
    const int rc = sqlite3_open_v2(path.c_str(), &db->db_, flags, nullptr);
    if (rc != SQLITE_OK) {
        std::string message = db->db_ ? sqlite3_errmsg(db->db_) : sqlite3_errstr(rc);
        const int ext = db->db_ ? sqlite3_extended_errcode(db->db_) : rc;
        if (db->db_) sqlite3_close_v2(db->db_);
        db->db_ = nullptr;
        throw Error("sqlite open failed (" + path + "): " + message, rc & 0xFF, ext);
    }
    sqlite3_extended_result_codes(db->db_, 1);
    sqlite3_busy_timeout(db->db_, options.busy_timeout_ms);
    // 1 値の上限 (DB の値も外部入力として扱う)。
    sqlite3_limit(db->db_, SQLITE_LIMIT_LENGTH, 256 * 1024 * 1024);
    sqlite3_limit(db->db_, SQLITE_LIMIT_SQL_LENGTH, 16 * 1024 * 1024);
    return db;
}

Database::~Database() { close(); }

bool Database::is_open() const {
    std::lock_guard lock(mutex_);
    return db_ != nullptr;
}

void Database::close() {
    std::lock_guard lock(mutex_);
    for (auto& [sql, stmt] : cache_) sqlite3_finalize(stmt);
    cache_.clear();
    if (db_) {
        sqlite3_close_v2(db_);
        db_ = nullptr;
    }
}

void Database::throw_error(std::string_view context) {
    const int ext = db_ ? sqlite3_extended_errcode(db_) : SQLITE_MISUSE;
    std::string message = db_ ? sqlite3_errmsg(db_) : "database is closed";
    throw Error(std::string(context) + ": " + message, ext & 0xFF, ext);
}

sqlite3_stmt* Database::prepare_cached(std::string_view sql) {
    if (!db_) throw Error("database is closed", SQLITE_MISUSE, SQLITE_MISUSE);
    auto it = cache_.find(std::string(sql));
    if (it != cache_.end()) {
        sqlite3_reset(it->second);
        sqlite3_clear_bindings(it->second);
        return it->second;
    }
    sqlite3_stmt* stmt = nullptr;
    const char* tail = nullptr;
    if (sqlite3_prepare_v3(db_, sql.data(), static_cast<int>(sql.size()), SQLITE_PREPARE_PERSISTENT, &stmt, &tail) != SQLITE_OK)
        throw_error("prepare");
    if (!stmt) throw Error("empty statement", SQLITE_MISUSE, SQLITE_MISUSE);
    // better-sqlite3 と同じく、prepare は 1 文だけ (残りが空白以外ならエラー)。
    for (const char* p = tail; p && *p; ++p) {
        if (*p != ' ' && *p != '\n' && *p != '\r' && *p != '\t' && *p != ';') {
            sqlite3_finalize(stmt);
            throw Error("The supplied SQL string contains more than one statement", SQLITE_MISUSE, SQLITE_MISUSE);
        }
    }
    if (cache_.size() >= 512) {
        for (auto& [k, s] : cache_) sqlite3_finalize(s);
        cache_.clear();
    }
    cache_.emplace(std::string(sql), stmt);
    return stmt;
}

void Database::bind(sqlite3_stmt* stmt, const Params& params) {
    auto bind_one = [&](int index, const Value& v) {
        int rc = SQLITE_OK;
        const auto& s = v.storage();
        if (std::holds_alternative<std::monostate>(s)) rc = sqlite3_bind_null(stmt, index);
        else if (const auto* i = std::get_if<std::int64_t>(&s)) rc = sqlite3_bind_int64(stmt, index, *i);
        else if (const auto* d = std::get_if<double>(&s)) rc = sqlite3_bind_double(stmt, index, *d);
        else if (const auto* t = std::get_if<std::string>(&s))
            rc = sqlite3_bind_text64(stmt, index, t->data(), t->size(), SQLITE_TRANSIENT, SQLITE_UTF8);
        else if (const auto* b = std::get_if<Blob>(&s))
            rc = sqlite3_bind_blob64(stmt, index, b->bytes.data(), b->bytes.size(), SQLITE_TRANSIENT);
        if (rc != SQLITE_OK) throw_error("bind");
    };
    const int count = sqlite3_bind_parameter_count(stmt);
    const auto& pos = params.positional();
    if (!params.named_values().empty()) {
        for (const auto& [name, v] : params.named_values()) {
            int index = 0;
            for (const char* prefix : {"@", ":", "$"}) {
                index = sqlite3_bind_parameter_index(stmt, (std::string(prefix) + name).c_str());
                if (index) break;
            }
            if (index == 0) throw Error("Missing named parameter \"" + name + "\" in statement", SQLITE_RANGE, SQLITE_RANGE);
            bind_one(index, v);
        }
        return;
    }
    if (static_cast<int>(pos.size()) != count)
        throw Error("Too " + std::string(static_cast<int>(pos.size()) < count ? "few" : "many") + " parameter values were provided",
                    SQLITE_RANGE, SQLITE_RANGE);
    for (int i = 0; i < count; ++i) bind_one(i + 1, pos[static_cast<std::size_t>(i)]);
}

namespace {
Value column_value(sqlite3_stmt* stmt, int i, lorica::input_budget* budget) {
    switch (sqlite3_column_type(stmt, i)) {
    case SQLITE_INTEGER: return Value(static_cast<long long>(sqlite3_column_int64(stmt, i)));
    case SQLITE_FLOAT: return Value(sqlite3_column_double(stmt, i));
    case SQLITE_TEXT: {
        const auto* p = reinterpret_cast<const char*>(sqlite3_column_text(stmt, i));
        const auto n = static_cast<std::size_t>(sqlite3_column_bytes(stmt, i));
        if (budget && !budget->consume(n, 0, 0)) throw Error("sql: result too large", SQLITE_TOOBIG, SQLITE_TOOBIG);
        return Value(std::string(p ? p : "", n));
    }
    case SQLITE_BLOB: {
        const auto* p = static_cast<const char*>(sqlite3_column_blob(stmt, i));
        const auto n = static_cast<std::size_t>(sqlite3_column_bytes(stmt, i));
        if (budget && !budget->consume(n, 0, 0)) throw Error("sql: result too large", SQLITE_TOOBIG, SQLITE_TOOBIG);
        return Value(Blob{std::string(p ? p : "", n)});
    }
    default: return Value();
    }
}

Row read_row(sqlite3_stmt* stmt, lorica::input_budget* budget) {
    Row row;
    const int n = sqlite3_column_count(stmt);
    for (int i = 0; i < n; ++i) {
        const char* name = sqlite3_column_name(stmt, i);
        row.push(name ? name : "", column_value(stmt, i, budget));
    }
    return row;
}

struct ResetOnExit {
    sqlite3_stmt* stmt;
    ~ResetOnExit() {
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
    }
};
} // namespace

void Database::exec(std::string_view sql) {
    std::lock_guard lock(mutex_);
    if (!db_) throw Error("database is closed", SQLITE_MISUSE, SQLITE_MISUSE);
    const std::string s(sql);
    char* err = nullptr;
    if (sqlite3_exec(db_, s.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
        std::string message = err ? err : "exec failed";
        sqlite3_free(err);
        const int ext = sqlite3_extended_errcode(db_);
        throw Error("exec: " + message, ext & 0xFF, ext);
    }
}

RunResult Database::run(std::string_view sql, const Params& params) {
    std::lock_guard lock(mutex_);
    sqlite3_stmt* stmt = prepare_cached(sql);
    ResetOnExit reset{stmt};
    bind(stmt, params);
    while (true) {
        const int rc = sqlite3_step(stmt);
        if (rc == SQLITE_DONE) break;
        if (rc == SQLITE_ROW) continue;
        throw_error("run");
    }
    return {sqlite3_changes64(db_), sqlite3_last_insert_rowid(db_)};
}

std::optional<Row> Database::get(std::string_view sql, const Params& params) {
    std::lock_guard lock(mutex_);
    sqlite3_stmt* stmt = prepare_cached(sql);
    ResetOnExit reset{stmt};
    bind(stmt, params);
    const int rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE) return std::nullopt;
    if (rc != SQLITE_ROW) throw_error("get");
    lorica::input_budget budget({QueryLimits{}.max_bytes, 1, 1, 1});
    return read_row(stmt, &budget);
}

std::vector<Row> Database::all(std::string_view sql, const Params& params, const QueryLimits& limits) {
    std::lock_guard lock(mutex_);
    sqlite3_stmt* stmt = prepare_cached(sql);
    ResetOnExit reset{stmt};
    bind(stmt, params);
    lorica::input_budget budget({limits.max_bytes, limits.max_rows, limits.max_rows, 1});
    std::vector<Row> rows;
    while (true) {
        const int rc = sqlite3_step(stmt);
        if (rc == SQLITE_DONE) break;
        if (rc != SQLITE_ROW) throw_error("all");
        if (!budget.consume(0, 1, 1)) throw Error("sql: too many rows", SQLITE_TOOBIG, SQLITE_TOOBIG);
        rows.push_back(read_row(stmt, &budget));
    }
    return rows;
}

void Database::each(std::string_view sql, const Params& params, const std::function<bool(const Row&)>& fn) {
    std::lock_guard lock(mutex_);
    sqlite3_stmt* stmt = prepare_cached(sql);
    ResetOnExit reset{stmt};
    bind(stmt, params);
    while (true) {
        const int rc = sqlite3_step(stmt);
        if (rc == SQLITE_DONE) break;
        if (rc != SQLITE_ROW) throw_error("each");
        lorica::input_budget budget({QueryLimits{}.max_bytes, 1, 1, 1});
        if (!fn(read_row(stmt, &budget))) break;
    }
}

std::optional<Value> Database::pragma_value(std::string_view pragma) {
    auto row = get("PRAGMA " + std::string(pragma));
    if (!row || row->size() == 0) return std::nullopt;
    return row->value(0);
}

void Database::pragma(std::string_view statement) { exec("PRAGMA " + std::string(statement)); }

bool Database::in_transaction() const {
    std::lock_guard lock(mutex_);
    return db_ && sqlite3_get_autocommit(db_) == 0;
}

std::int64_t Database::changes() const {
    std::lock_guard lock(mutex_);
    return db_ ? sqlite3_changes64(db_) : 0;
}

bool Database::table_exists(std::string_view table) {
    return get("SELECT 1 AS x FROM sqlite_master WHERE type='table' AND name = ?", {std::string(table)}).has_value();
}

std::vector<std::string> Database::table_columns(std::string_view table) {
    std::vector<std::string> out;
    std::string quoted = "\"";
    for (char c : table) {
        if (c == '"') quoted += "\"\"";
        else quoted.push_back(c);
    }
    quoted += "\"";
    for (const auto& row : all("PRAGMA table_info(" + quoted + ")")) out.push_back(row.str("name"));
    return out;
}

} // namespace di::sql
