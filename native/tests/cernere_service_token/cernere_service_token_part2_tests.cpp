// cernere_service_token のテスト (2/3)。準備は cernere_service_token_support.hpp。
#include "cernere_service_token_support.hpp"

using namespace cernere_service_token_support;

TEST_CASE("public key loader over real HTTP: response size limit") {
    auto router = std::make_shared<http::Router>();
    router->get("/.well-known/cernere-public-key", [](http::Request&) {
        return http::Response::json(Json{{"keys", Json::array()}, {"pad", std::string(kPublicKeyMaxResponseBytes, 'x')}});
    });
    http::ServerOptions so;
    so.port = 0;
    http::Server server(so, router);
    server.start();
    const std::string base = "http://127.0.0.1:" + std::to_string(server.bound_port());
    auto loader = create_cernere_public_key_loader({[&] { return base; }, {}, {}});
    CHECK_THROWS(loader());  // 上限超過は失敗 (読み込まない)
    server.stop();
}

TEST_CASE("service token client caches until exp - 60s and reports failure reasons") {
    int issued = 0;
    std::int64_t clock = 1'800'000'000'000;
    std::vector<Json> bodies;
    std::vector<std::string> urls;
    auto provider = create_service_token_provider({"voluptas",
                                                   [] { return ServiceTokenClientConfig{"https://cernere.test", "client-1", "secret-1"}; },
                                                   [&](const http::ClientRequest& req) {
                                                       ++issued;
                                                       urls.push_back(req.url);
                                                       bodies.push_back(parse_json_or_throw(req.body));
                                                       CHECK_EQ(req.max_redirects, 0);
                                                       return json_response(Json{{"tokenType", "service"}, {"accessToken", "svc-token-" + std::to_string(issued)}, {"expiresIn", 900}});
                                                   },
                                                   [&] { return clock; }});
    CHECK_EQ(provider().token, std::string("svc-token-1"));
    clock += (900 - 61) * 1000;
    CHECK_EQ(provider().token, std::string("svc-token-1"));
    clock += 2 * 1000;
    CHECK_EQ(provider().token, std::string("svc-token-2"));
    CHECK_EQ(urls[0], std::string("https://cernere.test/api/auth/service-token"));
    CHECK_EQ(json_stringify(bodies[0]), std::string(R"({"client_id":"client-1","client_secret":"secret-1","target_project_key":"voluptas"})"));

    auto failure = [](ServiceTokenClientConfig cfg, std::function<http::ClientResponse(const http::ClientRequest&)> f) {
        return create_service_token_provider({"voluptas", [cfg] { return cfg; }, std::move(f), {}})().reason;
    };
    const ServiceTokenClientConfig creds{"https://cernere.test", "c", "s"};
    auto never = [](const http::ClientRequest&) -> http::ClientResponse { throw std::runtime_error("credentials must not be sent"); };
    CHECK_EQ(failure({creds.baseUrl, "c", ""}, never), std::string("credentials_missing"));
    CHECK_EQ(failure({"http://cernere.test", "c", "s"}, never), std::string("base_url_insecure"));
    CHECK_EQ(failure({"ftp://cernere.test", "c", "s"}, never), std::string("base_url_insecure"));
    CHECK_EQ(failure({"  ", "c", "s"}, never), std::string("base_url_missing"));
    CHECK_EQ(failure({"not a url", "c", "s"}, never), std::string("base_url_missing"));
    CHECK_EQ(failure({"http://localhost:7000", "c", "s"}, [](const http::ClientRequest&) { return json_response(Json::object(), 401); }),
             std::string("unauthorized"));
    CHECK_EQ(failure({"http://[::1]:7000", "c", "s"}, [](const http::ClientRequest&) { return json_response(Json::object(), 403); }),
             std::string("scope_undeclared"));
    CHECK_EQ(failure(creds, [](const http::ClientRequest&) { return json_response(Json::object(), 404); }), std::string("target_not_found"));
    CHECK_EQ(failure(creds, [](const http::ClientRequest&) { return json_response(Json::object(), 502); }), std::string("issuer_error"));
    CHECK_EQ(failure(creds, [](const http::ClientRequest&) -> http::ClientResponse { throw std::runtime_error("fetch failed"); }),
             std::string("network"));
    CHECK_EQ(failure(creds, [](const http::ClientRequest&) { return http::ClientResponse{}; }), std::string("network"));
    CHECK_EQ(failure(creds, [](const http::ClientRequest&) {
                 http::ClientResponse r = json_response(Json::object());
                 r.body = "{bad";
                 return r;
             }),
             std::string("malformed_response"));
    CHECK_EQ(failure(creds, [](const http::ClientRequest&) { return json_response(Json{{"accessToken", "x"}, {"expiresIn", 0}}); }),
             std::string("malformed_response"));
    CHECK_EQ(failure(creds, [](const http::ClientRequest&) { return json_response(Json{{"accessToken", ""}, {"expiresIn", 5}}); }),
             std::string("malformed_response"));
    const auto env_cfg = service_token_client_config_from_env(std::map<std::string, std::string>{{"CERNERE_BASE_URL", "https://c"}});
    CHECK_EQ(env_cfg.baseUrl, std::string("https://c"));
    CHECK(env_cfg.clientId.empty());
}

TEST_CASE("Voluptas bearer: service token first, fixed token only on failure, never logged") {
    using flow::resolve_voluptas_bearer;
    std::vector<std::string> logs;
    auto log = [&](const std::string& l) { logs.push_back(l); };
    CHECK_EQ(resolve_voluptas_bearer({ServiceTokenProvider([] { return ServiceTokenResult{true, "svc", ""}; }), "fixed", log}),
             std::string("svc"));
    CHECK(logs.empty());
    CHECK_EQ(resolve_voluptas_bearer({ServiceTokenProvider([] { return ServiceTokenResult{false, "", "scope_undeclared"}; }),
                                      "fixed-secret-value", log}),
             std::string("fixed-secret-value"));
    CHECK_EQ(logs.size(), std::size_t{1});
    CHECK(logs[0].find("reason=scope_undeclared") != std::string::npos);
    CHECK(logs[0].find("fixed-secret-value") == std::string::npos);
    try {
        resolve_voluptas_bearer({ServiceTokenProvider([] { return ServiceTokenResult{false, "", "network"}; }), "", log});
        CHECK(false);
    } catch (const std::exception& e) {
        CHECK(std::string(e.what()).find("reason=network") != std::string::npos);
    }
    CHECK_THROWS(resolve_voluptas_bearer({std::nullopt, "", log}));
    CHECK_EQ(resolve_voluptas_bearer({std::nullopt, "fixed", log}), std::string("fixed"));
}

DI_TEST_MAIN()
