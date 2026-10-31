#include "check.hpp"

#include <cmath>

#include "base/json.hpp"

using di::Json;

TEST_CASE("stringify matches JSON.stringify for numbers") {
    CHECK_EQ(di::json_stringify(Json(1.0)), std::string("1"));
    CHECK_EQ(di::json_stringify(Json(0.1)), std::string("0.1"));
    CHECK_EQ(di::json_stringify(Json(1e21)), std::string("1e+21"));
    CHECK_EQ(di::json_stringify(Json(1e-7)), std::string("1e-7"));
    CHECK_EQ(di::json_stringify(Json(123456789012.5)), std::string("123456789012.5"));
    CHECK_EQ(di::json_stringify(Json(-0.000001)), std::string("-0.000001"));
    CHECK_EQ(di::json_stringify(Json(std::nan(""))), std::string("null"));
}

TEST_CASE("stringify keeps insertion order and escapes like JS") {
    Json j = Json::object();
    j["b"] = 1;
    j["a"] = "x\"\n\x01日本";
    CHECK_EQ(di::json_stringify(j), std::string("{\"b\":1,\"a\":\"x\\\"\\n\\u0001日本\"}"));
    CHECK_EQ(di::json_stringify_pretty(Json{{"a", Json::array({1, 2})}, {"e", Json::object()}}),
             std::string("{\n  \"a\": [\n    1,\n    2\n  ],\n  \"e\": {}\n}"));
}

TEST_CASE("invalid utf-8 is replaced when stringifying") {
    CHECK_EQ(di::json_stringify(Json(std::string("a\xff" "b"))), std::string("\"a\xEF\xBF\xBD" "b\""));
}

TEST_CASE("parse_json enforces byte limit at boundary") {
    di::JsonLimits limits;
    limits.max_bytes = 7;
    CHECK(di::parse_json("[1,2,3]", limits).has_value());     // ちょうど 7 バイト
    CHECK(!di::parse_json("[1,2,34]", limits).has_value());   // 8 バイト
}

TEST_CASE("parse_json enforces depth limit") {
    di::JsonLimits limits;
    limits.max_depth = 3;
    CHECK(di::parse_json("[[[1]]]", limits).has_value());
    std::string err;
    CHECK(!di::parse_json("[[[[1]]]]", limits, &err).has_value());
    CHECK(err.find("deep") != std::string::npos);
    // 深い入れ子 (スタックを使い切る深さ) も既定の上限で拒否する。
    std::string deep(100000, '[');
    CHECK(!di::parse_json(deep).has_value());
}

TEST_CASE("parse_json enforces element limit") {
    di::JsonLimits limits;
    limits.max_elements = 4;  // 配列 1 + 要素 3
    CHECK(di::parse_json("[1,2,3]", limits).has_value());
    CHECK(!di::parse_json("[1,2,3,4]", limits).has_value());
}

TEST_CASE("parse_json rejects broken input") {
    CHECK(!di::parse_json("{").has_value());
    CHECK(!di::parse_json("").has_value());
    CHECK(!di::parse_json("{\"a\":}").has_value());
}

TEST_CASE("accessors fall back on wrong types") {
    Json j = Json::parse(R"({"s":"x","n":2.5,"i":3,"b":true,"a":[1],"o":{"k":1}})");
    CHECK_EQ(di::json_string(j, "s"), std::string("x"));
    CHECK_EQ(di::json_string(j, "n", "fb"), std::string("fb"));
    CHECK_EQ(di::json_number(j, "n"), 2.5);
    CHECK_EQ(di::json_int(j, "i"), 3);
    CHECK(di::json_bool(j, "b"));
    CHECK_EQ(di::json_array(j, "a").size(), std::size_t(1));
    CHECK(di::json_array(j, "o").empty());
    CHECK(di::json_object(j, "o").contains("k"));
    CHECK(!di::json_truthy(Json("")));
    CHECK(di::json_truthy(Json("0")));
}

DI_TEST_MAIN()
