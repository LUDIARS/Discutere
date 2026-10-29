#pragma once
// JSON: nlohmann::ordered_json (JS と同じくキーの挿入順を保つ) と、
// 外部入力を lorica::input_budget で課金しながら読む parse_json、
// JS の JSON.stringify と同じ書式で書く json_stringify。
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace di {

using Json = nlohmann::ordered_json;

/** 外部 JSON を読むときの上限。既定は HTTP 本文 1 件ぶん。 */
struct JsonLimits {
    std::size_t max_bytes = 1u << 20;     // 入力の大きさ
    std::size_t max_elements = 200'000;   // 値 (object/array/scalar) の総数
    std::size_t max_depth = 64;           // 入れ子の深さ
};

class JsonLimitError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/**
 * 上限つきで JSON を読む。壊れた入力・上限超過は std::nullopt (error に理由)。
 * 大きさ → 要素数 → 深さの順に課金し、上限を超えた時点で読むのをやめる。
 */
std::optional<Json> parse_json(std::string_view text, const JsonLimits& limits = {}, std::string* error = nullptr);

/** parse_json と同じ。失敗は std::runtime_error。 */
Json parse_json_or_throw(std::string_view text, const JsonLimits& limits = {});

/** JS の JSON.stringify(value) と同じ書式 (整数値の double は整数で書く、非 ASCII はそのまま)。 */
std::string json_stringify(const Json& value);

/** JS の JSON.stringify(value, null, indent)。 */
std::string json_stringify_pretty(const Json& value, int indent = 2);

/** JS の Number#toString と同じ書式で数値を書く (NaN/Infinity は "NaN"/"Infinity")。 */
std::string js_number_to_string(double value);

// ─── 取り出しの補助 (型が違う・無いときは既定値) ─────────────────────
std::string json_string(const Json& object, std::string_view key, std::string_view fallback = {});
std::optional<std::string> json_opt_string(const Json& object, std::string_view key);
double json_number(const Json& object, std::string_view key, double fallback = 0);
std::optional<double> json_opt_number(const Json& object, std::string_view key);
std::int64_t json_int(const Json& object, std::string_view key, std::int64_t fallback = 0);
bool json_bool(const Json& object, std::string_view key, bool fallback = false);
std::optional<bool> json_opt_bool(const Json& object, std::string_view key);
/** object[key] が配列ならそれ、違えば空配列。 */
const Json& json_array(const Json& object, std::string_view key);
/** object[key] が object ならそれ、違えば空 object。 */
const Json& json_object(const Json& object, std::string_view key);
/** JS の truthy 判定 (null/false/0/NaN/"" が偽)。 */
bool json_truthy(const Json& value);

} // namespace di
