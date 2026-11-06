// gray-matter との互換 (golden: native/tests/golden/front_matter.gen.ts が記録した Node の出力)。
#include "check.hpp"

#include <cmath>

#include "base/fsutil.hpp"
#include "lib/front_matter/front_matter.hpp"

using namespace di;
namespace fm = di::lib::front_matter;

namespace {
// JSON.stringify と同じく ±Infinity / NaN は null として比べる。
Json normalize(const Json& j) {
    if (j.is_number_float() && !std::isfinite(j.get<double>())) return nullptr;
    if (j.is_object()) {
        Json o = Json::object();
        for (auto it = j.begin(); it != j.end(); ++it) o[it.key()] = normalize(it.value());
        return o;
    }
    if (j.is_array()) {
        Json a = Json::array();
        for (const auto& v : j) a.push_back(normalize(v));
        return a;
    }
    return j;
}
} // namespace

TEST_CASE("parse matches gray-matter golden cases") {
    auto text = fs::read_file(test::fixtures_dir() + "/front_matter/parse.json");
    CHECK(text.has_value());
    auto cases = parse_json(*text, {8u << 20, 1'000'000, 64});
    CHECK(cases.has_value());
    int n = 0;
    for (const auto& c : *cases) {
        const std::string input = c["input"].get<std::string>();
        std::string error;
        auto m = fm::parse(input, &error);
        const std::string label = "case " + std::to_string(n++) + ": " + json_stringify(Json(input));
        if (!c["ok"].get<bool>()) {
            CHECK_EQ(m.has_value(), false);
            continue;
        }
        if (!m) throw test::Failure(label + " failed: " + error);
        CHECK_EQ(json_stringify(normalize(m->data)), json_stringify(c["data"]));
        CHECK_EQ(m->content, c["content"].get<std::string>());
    }
}

TEST_CASE("unsupported YAML is rejected") {
    for (const char* bad : {"---\na: &x 1\nb: *x\n---\n", "---\na: !!str x\n---\n", "---\n? complex\n: v\n---\n",
                            "---\na: 1\na: 2\n---\n", "---\nk: 1:20\nx\n---\n", "---\n\ta: 1\n---\n"}) {
        std::string error;
        CHECK(!fm::parse(bad, &error).has_value());
    }
}

TEST_CASE("limits: depth, elements and size") {
    std::string deep = "---\n";
    for (int i = 0; i < 40; ++i) deep += std::string(static_cast<std::size_t>(i * 2), ' ') + "k:\n";
    deep += "---\n";
    CHECK(!fm::parse(deep).has_value());
    fm::Limits limits;
    limits.max_elements = 5;
    CHECK(!fm::parse("---\na: 1\nb: 2\nc: 3\nd: 4\n---\n", nullptr, limits).has_value());
    limits = {};
    limits.max_bytes = 10;
    CHECK(!fm::parse("---\nabcdefghijk: 1\n---\n", nullptr, limits).has_value());
}

DI_TEST_MAIN()
