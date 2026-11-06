#pragma once
// w4-Q のテスト共通の補助: 一時ディレクトリに向けた環境 (KG / flow DB / workspace)、fixture、UUID の伏せ字、要求の組み立て。
#include <ctime>
#include <memory>
#include <string>

#include "base/env.hpp"
#include "base/fsutil.hpp"
#include "base/json.hpp"
#include "check.hpp"
#include "config.hpp"
#include "core/index.hpp"
#include "core/test_util.hpp"
#include "flow/db/connection.hpp"
#include "infra/http.hpp"

namespace w4q {

using di::Json;

inline Json load_fixture(const std::string& name) {
    auto text = di::fs::read_file(di::test::fixtures_dir() + "/w4q/" + name, 16u << 20);
    if (!text) throw di::test::Failure("fixture missing: " + name);
    return di::parse_json_or_throw(*text, {16u << 20, 2'000'000, 64});
}

/** UUID v4 を出てきた順に <U1> <U2> ... に置き換える (std::regex を使わない線形走査)。 */
inline std::string mask_uuids(const std::string& s) {
    auto is_hex = [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); };
    std::string out;
    int n = 0;
    for (std::size_t i = 0; i < s.size();) {
        if (i + 36 <= s.size()) {
            bool ok = true;
            for (std::size_t k = 0; k < 36 && ok; ++k) {
                const char c = s[i + k];
                ok = (k == 8 || k == 13 || k == 18 || k == 23) ? c == '-' : is_hex(c);
            }
            if (ok) {
                out += "<U" + std::to_string(++n) + ">";
                i += 36;
                continue;
            }
        }
        out.push_back(s[i++]);
    }
    return out;
}

/** active KG / flow DB / workspace "ws" / persona-engine DB を一時ディレクトリへ向け、TZ を UTC に固定する。 */
struct Env {
    di::test::TempDir dir;
    std::string kg;
    std::string flow;
    explicit Env(const std::string& workspace = "ws") {
        kg = dir.file("kg.kuzu");
        flow = dir.file("flow.db");
        di::env::set_for_test("DISCUTERE_CONFIG", std::string_view(dir.file("no-config.json")));
        di::env::set_for_test("DISCATIER_KUZU_PATH", std::string_view(kg));
        di::env::set_for_test("DATABASE_PATH", std::string_view(flow));
        di::env::set_for_test("DISCATIER_WORKSPACE", std::string_view(workspace));
        di::env::set_for_test("DISCUTERE_PERSONA_ENGINE_DB", std::string_view(dir.file("pe.db")));
        di::env::set_for_test("TZ", std::string_view("UTC"));
#ifndef _WIN32
        tzset();
#endif
        di::config::reset_config_for_test();
        di::flow::db::reset_flow_db_for_test();
    }
    ~Env() {
        di::flow::db::reset_flow_db_for_test();
        di::config::reset_config_for_test();
    }
    std::unique_ptr<di::core::Core> core() { return di::core::create_core(kg, dir.file("events.jsonl")); }
    /** KG に SQL を流す (setup)。 */
    void kg_exec(const Json& sqls) {
        auto c = core();
        for (const auto& s : sqls) c->raw().exec(s.get<std::string>());
        c->close();
    }
    void flow_exec(const Json& sqls) {
        auto db = di::flow::db::get_flow_db();
        for (const auto& s : sqls) db->exec(s.get<std::string>());
    }
};

inline di::http::Request make_request(std::string method, const std::string& target, const Json& headers = Json::object(),
                                      std::string remote = "", std::string body = "") {
    di::http::Request r;
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

inline Json body_json(const di::http::Response& res) {
    auto parsed = di::parse_json(res.body, {16u << 20, 2'000'000, 64});
    return parsed ? *parsed : Json(nullptr);
}

} // namespace w4q
