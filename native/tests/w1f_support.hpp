#pragma once
// w1-F のテストで共有する小さな補助 (fixture の読み込み・要求の組み立て)。
#include <string>
#include <utility>
#include <vector>

#include "base/fsutil.hpp"
#include "base/json.hpp"
#include "check.hpp"
#include "infra/http.hpp"

namespace di::test::w1f {

inline Json load_fixture(const std::string& name) {
    auto text = fs::read_file(fixtures_dir() + "/w1f/" + name, 32u << 20);
    if (!text) throw Failure("fixture missing: " + name);
    auto parsed = parse_json(*text, {32u << 20, 4'000'000, 64});
    if (!parsed) throw Failure("fixture broken: " + name);
    return *parsed;
}

/** "/a/b?x=1" から Request を作る。 */
inline http::Request make_request(std::string method, const std::string& target, const Json& headers = Json::object(),
                                  std::string remote = "", std::string body = "") {
    http::Request r;
    r.method = std::move(method);
    r.target = target;
    const auto q = target.find('?');
    r.path = target.substr(0, q);
    r.query_string = q == std::string::npos ? "" : target.substr(q + 1);
    if (headers.is_object())
        for (auto it = headers.begin(); it != headers.end(); ++it) r.headers.add(it.key(), it.value().get<std::string>());
    r.remote_address = std::move(remote);
    r.body = std::move(body);
    return r;
}

inline Json body_json(const http::Response& res) {
    auto parsed = parse_json(res.body);
    return parsed ? *parsed : Json(nullptr);
}

} // namespace di::test::w1f
