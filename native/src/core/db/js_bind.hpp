#pragma once
// better-sqlite3 に JS の値を bind したときと同じ変換 (core のデータ層の局所的な補助)。
//   number → REAL (better-sqlite3 は JS の number を常に double で bind する。INTEGER 列なら整数で保存される)
//   string → TEXT / null・undefined → NULL
//   boolean / object / array → TypeError (better-sqlite3 と同じく bind を拒否する)
#include <stdexcept>

#include "base/json.hpp"
#include "base/sqlite.hpp"

namespace di::core::db {

class JsBindError : public std::invalid_argument {
public:
    JsBindError() : std::invalid_argument("SQLite3 can only bind numbers, strings, bigints, buffers, and null") {}
};

inline sql::Value js_bind(const Json& v) {
    switch (v.type()) {
    case Json::value_t::null:
    case Json::value_t::discarded:
        return sql::Value();
    case Json::value_t::number_integer:
    case Json::value_t::number_unsigned:
    case Json::value_t::number_float:
        return sql::Value(v.get<double>());
    case Json::value_t::string:
        return sql::Value(v.get<std::string>());
    default:
        throw JsBindError();
    }
}

/** object[key] (無い・object でなければ null。TS の (payload as any).key の undefined)。 */
inline const Json& js_field(const Json& object, const char* key) {
    static const Json kNull;
    if (!object.is_object()) return kNull;
    auto it = object.find(key);
    return it == object.end() ? kNull : *it;
}

} // namespace di::core::db
