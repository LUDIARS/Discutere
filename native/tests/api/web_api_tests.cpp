// 画面の配信 (native/web/** が Node の文字列と 1 バイトも違わない) と api の小さなルート / 純関数。
// golden: fixtures/w1f/web_assets.json (sha256) と consensus.json。
#include "check.hpp"
#include "w1f_support.hpp"

#include "api/conclusion_publication_editor.hpp"
#include "api/consensus_persona_aggregation.hpp"
#include "api/dashboard_routes.hpp"
#include "api/top_page_routes.hpp"
#include "api/tuning_routes.hpp"
#include "base/crypto.hpp"
#include "base/strings.hpp"
#include "base/web_assets.hpp"
#include "flow/web/page.hpp"

using namespace di;
using namespace di::test::w1f;

TEST_CASE("embedded pages are byte-identical to the Node output") {
    const Json fx = load_fixture("web_assets.json");
    for (const auto& a : fx) {
        const auto* asset = web::find(a["name"].get<std::string>());
        if (!asset) throw test::Failure("missing asset " + a["name"].get<std::string>());
        CHECK_EQ(asset->body.size(), a["bytes"].get<std::size_t>());
        CHECK_EQ(crypto::sha256_hex(asset->body), a["sha256"].get<std::string>());
        CHECK_EQ(asset->content_type, "text/html; charset=UTF-8");
    }
    CHECK_EQ(crypto::sha256_hex(flow::web::flow_html()), fx[0]["sha256"].get<std::string>());
    CHECK_EQ(crypto::sha256_hex(api::publication_editor_html()), fx[1]["sha256"].get<std::string>());
}

TEST_CASE("routes serve the pages (top: no guard, dashboard: admin guard, tuning: no guard)") {
    const Json fx = load_fixture("web_assets.json");
    auto sha_of = [&](const std::string& name) {
        for (const auto& a : fx)
            if (a["name"] == name) return a["sha256"].get<std::string>();
        return std::string();
    };
    auto top = make_request("GET", "/", Json::object(), "192.168.1.5");
    auto res = api::top_page_routes().handle(top);
    CHECK_EQ(res.status, 200);
    CHECK_EQ(res.headers.get("Content-Type").value(), "text/html; charset=UTF-8");
    CHECK_EQ(crypto::sha256_hex(res.body), sha_of("api/top-page.html"));

    auto dash = make_request("GET", "/admin/dashboard", Json{{"X-User-Id", "a"}, {"X-User-Role", "admin"}});
    res = api::dashboard_routes().handle(dash);
    CHECK_EQ(res.status, 200);
    CHECK_EQ(crypto::sha256_hex(res.body), sha_of("api/dashboard.html"));
    auto anon = make_request("GET", "/admin/dashboard");
    CHECK_EQ(api::dashboard_routes().handle(anon).status, 401);

    auto tuning = make_request("GET", "/admin/tuning", Json::object(), "10.0.0.1");
    res = api::tuning_routes().handle(tuning);
    CHECK_EQ(res.status, 200);
    CHECK_EQ(crypto::sha256_hex(res.body), sha_of("api/tuning.html"));
    auto head = make_request("HEAD", "/", Json::object());
    res = api::top_page_routes().handle(head);
    CHECK_EQ(res.status, 200);
    CHECK(res.body.empty());
}

TEST_CASE("aggregate_persona_consensus matches Node (consensus-persona-aggregation.test.ts + golden)") {
    const Json fx = load_fixture("consensus.json");
    std::vector<api::PersonaConsensusSource> rows;
    for (const auto& r : fx["rows"]) {
        api::PersonaConsensusSource s;
        s.id = r["id"].get<std::string>();
        s.personaId = r["personaId"].get<std::string>();
        s.displayName = r["displayName"].get<std::string>();
        for (const auto& t : r["traits"]) s.traits.push_back(t.get<std::string>());
        s.content = r["content"].get<std::string>();
        s.postedAt = r["postedAt"].get<double>();
        if (!r["agree"].is_null()) s.agree = r["agree"].get<bool>();
        if (!r["score"].is_null()) s.score = r["score"].get<double>();
        if (!r["reasoning"].is_null()) s.reasoning = r["reasoning"].get<std::string>();
        s.opinionScore = r["opinionScore"].get<double>();
        rows.push_back(std::move(s));
    }
    Json got = Json::array();
    for (const auto& p : api::aggregate_persona_consensus(rows)) got.push_back(api::to_json(p));
    CHECK_EQ(json_stringify(got), json_stringify(fx["aggregated"]));
    CHECK(api::aggregate_persona_consensus({}).empty());

    std::vector<std::vector<std::optional<bool>>> ratio_inputs{{}, {std::nullopt}, {true, false, std::nullopt}, {true, true, false}};
    for (std::size_t i = 0; i < ratio_inputs.size(); ++i) {
        std::vector<api::PersonaConsensusUtterance> us;
        for (auto a : ratio_inputs[i]) {
            api::PersonaConsensusUtterance u;
            u.agree = a;
            us.push_back(u);
        }
        CHECK_EQ(api::ratio_of_agreed(us), fx["ratios"][i].get<double>());
    }
}

DI_TEST_MAIN()
