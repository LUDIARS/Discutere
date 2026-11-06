// matter.stringify / js-yaml safeDump との互換 (golden: native/tests/golden/w1-C-front_matter_dump.gen.ts)。
#include "check.hpp"

#include <cmath>
#include <limits>

#include "base/fsutil.hpp"
#include "lib/front_matter/front_matter.hpp"

using namespace di;
namespace fm = di::lib::front_matter;

TEST_CASE("stringify and dump_yaml match gray-matter golden cases") {
    auto text = fs::read_file(test::fixtures_dir() + "/front_matter/stringify.json");
    CHECK(text.has_value());
    auto cases = parse_json(*text, {16u << 20, 1'000'000, 64});
    CHECK(cases.has_value());
    int n = 0;
    for (const auto& c : *cases) {
        const std::string label = "case " + std::to_string(n++);
        const std::string content = c["content"].get<std::string>();
        const std::string yaml = fm::dump_yaml(c["data"]);
        if (yaml != c["yaml"].get<std::string>())
            throw test::Failure(label + " yaml\n  actual:   " + json_stringify(Json(yaml)) +
                                "\n  expected: " + json_stringify(c["yaml"]));
        const std::string out = fm::stringify(content, c["data"]);
        if (out != c["output"].get<std::string>())
            throw test::Failure(label + " output\n  actual:   " + json_stringify(Json(out)) +
                                "\n  expected: " + json_stringify(c["output"]));
    }
    CHECK(n > 150);
}

TEST_CASE("keys are ordered like JS Object.keys (array-index keys first)") {
    Json data = Json::object();
    data["b"] = 1;
    data["2"] = 2;
    data["1"] = 3;
    data["01"] = 4;
    data["4294967295"] = 5;
    // Node: matter.stringify("", {b:1,"2":2,"1":3,"01":4,"4294967295":5})
    CHECK_EQ(fm::dump_yaml(data), std::string("'1': 3\n'2': 2\nb: 1\n'01': 4\n'4294967295': 5\n"));
}

TEST_CASE("numbers JSON cannot carry: -0, NaN, Infinity") {
    Json d = Json::object();
    d["z"] = -0.0;
    d["n"] = std::numeric_limits<double>::quiet_NaN();
    d["i"] = std::numeric_limits<double>::infinity();
    d["m"] = -std::numeric_limits<double>::infinity();
    d["e"] = 1e-7;
    CHECK_EQ(fm::dump_yaml(d), std::string("z: -0.0\n'n': .nan\ni: .inf\nm: -.inf\ne: 1.e-7\n"));
}

TEST_CASE("empty data writes only content; null data keeps parsed matter") {
    CHECK_EQ(fm::stringify("", Json::object()), std::string("\n"));
    CHECK_EQ(fm::stringify("body", Json::object()), std::string("body\n"));
    CHECK_EQ(fm::stringify("---\na: 1\n---\nx", Json()), std::string("---\na: 1\n---\nx\n"));
    Json arr = Json::array({"p", "q"});
    CHECK_EQ(fm::stringify("", arr), std::string("---\n'0': p\n'1': q\n---\n\n"));
}

TEST_CASE("boundaries: huge strings, deep nesting and unreadable content") {
    // 長い文字列 (1MiB) も線形で折り返す
    std::string big;
    for (int i = 0; i < 100000; ++i) big += "word ";
    Json d = Json::object();
    d["big"] = big;
    const std::string y = fm::dump_yaml(d);
    CHECK(y.rfind("big: >-\n", 0) == 0);
    // 入れ子 256 段までは書ける、超えたら拒否
    Json deep = Json::array();
    Json* cur = &deep;
    for (int i = 0; i < 255; ++i) {
        cur->push_back(Json::array());
        cur = &(*cur)[0];
    }
    CHECK(!fm::dump_yaml(deep).empty());
    Json deeper = Json::array();
    cur = &deeper;
    for (int i = 0; i < 300; ++i) {
        cur->push_back(1);
        cur->push_back(Json::array());
        cur = &(*cur)[1];
    }
    CHECK_THROWS(fm::dump_yaml(deeper));
    // 読めない front matter の content は gray-matter と同じく例外
    CHECK_THROWS(fm::stringify("---\na: &x 1\n---\n", Json::object()));
    // 1 キーが 1024 文字を超えると explicit key ("? ")
    Json k = Json::object();
    k[std::string(1100, 'k')] = 1;
    CHECK(fm::dump_yaml(k).rfind("? ", 0) == 0);
}

DI_TEST_MAIN()
