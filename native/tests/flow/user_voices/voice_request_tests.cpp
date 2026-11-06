// w2-H: src/flow/user-voices/{request-client,preparation}.ts の移植の確認
// (tests/flow/voice-request-client.test.ts / voice-preparation.test.ts + golden)。Voluptas は偽サーバ / 偽 fetch。
#include "../w2h_support.hpp"

#include <atomic>
#include <memory>

#include "flow/user_voices/preparation.hpp"
#include "flow/user_voices/request_client.hpp"
#include "infra/http_server.hpp"

using namespace di;
using namespace di::flow::user_voices;

namespace {

cernere_service_token::ServiceTokenProvider token_counter(int* tokens) {
    return [tokens] {
        ++*tokens;
        return cernere_service_token::ServiceTokenResult{true, "test-token", ""};
    };
}

http::ClientResponse json_response(const Json& body, int status = 200) {
    http::ClientResponse r;
    r.transport_ok = true;
    r.status = status;
    r.body = json_stringify(body);
    return r;
}

http::ClientResponse status_response(int status) {
    http::ClientResponse r;
    r.transport_ok = true;
    r.status = status;
    return r;
}

http::ClientResponse transport_error() {
    http::ClientResponse r;
    r.error = "offline";
    r.error_kind = http::FetchErrorKind::network;
    return r;
}

template <class Fn>
void expect_failure(Fn&& fn, const std::string& kind, bool delivery_unknown) {
    try {
        fn();
        CHECK(false);
    } catch (const VoiceRequestFailure& f) {
        CHECK_EQ(f.kind(), kind);
        CHECK_EQ(f.delivery_unknown(), delivery_unknown);
    }
}

} // namespace

TEST_CASE("request client: down before auth, manual redirects, status mapping (voice-request-client.test.ts)") {
    int tokens = 0;
    auto down = create_voice_request_client([] { return std::string("http://localhost:8892"); }, token_counter(&tokens),
                                            [](const http::ClientRequest&) { return transport_error(); });
    expect_failure([&] { down.submit("di:1", "theme"); }, "down", false);
    CHECK_EQ(tokens, 0);

    std::vector<http::ClientRequest> calls;
    auto client = create_voice_request_client([] { return std::string("http://localhost:8892/ignored/path"); }, token_counter(&tokens),
                                              [&](const http::ClientRequest& req) {
                                                  calls.push_back(req);
                                                  return req.method == "POST"
                                                             ? json_response(Json{{"data", {{"request", {{"requestId", "di:1"}, {"status", "accepted"}}}}}})
                                                             : json_response(Json{{"ok", true}});
                                              });
    const auto receipt = client.submit("di:1", "theme");
    CHECK(receipt.accepted);
    CHECK_EQ(receipt.requestId, std::string("di:1"));
    CHECK_EQ(calls[0].url, std::string("http://localhost:8892/health"));
    CHECK_EQ(calls[1].url, std::string("http://localhost:8892/api/personas/impression-requests"));
    CHECK_EQ(calls[1].max_redirects, 0);
    CHECK_EQ(calls[1].max_response_bytes, kVoiceRequestMaxResponseBytes);
    CHECK_EQ(calls[1].body, std::string(R"({"requestId":"di:1","theme":"theme"})"));
    CHECK_EQ(*calls[1].headers.get("authorization"), std::string("Bearer test-token"));

    for (int status : {401, 403, 503}) {
        auto rejected = create_voice_request_client([] { return std::string("http://localhost:8892"); }, token_counter(&tokens),
                                                    [status](const http::ClientRequest& req) {
                                                        return req.method == "POST" ? status_response(status) : json_response(Json{{"ok", true}});
                                                    });
        expect_failure([&] { rejected.submit("di:1", "theme"); }, status == 503 ? "down" : "auth", status == 503);
    }
    auto ambiguous = create_voice_request_client([] { return std::string("http://localhost:8892"); }, token_counter(&tokens),
                                                 [](const http::ClientRequest& req) {
                                                     return req.method == "POST" ? transport_error() : json_response(Json{{"ok", true}});
                                                 });
    expect_failure([&] { ambiguous.submit("di:1", "theme"); }, "down", true);
}

TEST_CASE("request client: configuration / auth / protocol / status lookup") {
    int tokens = 0;
    const auto ok_fetch = [](const http::ClientRequest&) { return json_response(Json{{"ok", true}}); };
    for (const char* bad : {"", "not a url", "http://voluptas.example", "https://user:pw@voluptas.example", "ftp://localhost"}) {
        auto c = create_voice_request_client([bad] { return std::string(bad); }, token_counter(&tokens), ok_fetch);
        expect_failure([&] { c.submit("di:1", "t"); }, "configuration", false);
    }
    CHECK_EQ(tokens, 0);
    auto health_bad = create_voice_request_client([] { return std::string("https://vo.example"); }, token_counter(&tokens),
                                                  [](const http::ClientRequest&) { return status_response(302); });
    expect_failure([&] { health_bad.submit("di:1", "t"); }, "protocol", false);
    auto no_token = create_voice_request_client([] { return std::string("http://[::1]:1"); },
                                                [] { return cernere_service_token::ServiceTokenResult{false, "", "credentials_missing"}; }, ok_fetch);
    expect_failure([&] { no_token.submit("di:1", "t"); }, "auth", false);

    std::string last_url;
    auto lookup = create_voice_request_client([] { return std::string("http://127.0.0.1:9"); }, token_counter(&tokens),
                                              [&](const http::ClientRequest& req) {
                                                  last_url = req.url;
                                                  if (req.url.find("missing") != std::string::npos) return status_response(404);
                                                  if (req.url.find("wrong") != std::string::npos)
                                                      return json_response(Json{{"data", {{"request", {{"requestId", "other"}, {"status", "accepted"}}}}}});
                                                  if (req.url.find("broken") != std::string::npos) {
                                                      auto r = status_response(200);
                                                      r.body = "{not json";
                                                      return r;
                                                  }
                                                  if (req.url.find("health") != std::string::npos) return json_response(Json{{"ok", true}});
                                                  return json_response(Json{{"data", {{"request", {{"requestId", "di:a b"}, {"status", "cancelled"}}}}}});
                                              });
    CHECK(!lookup.status("di:a b").accepted);
    CHECK_EQ(last_url, std::string("http://127.0.0.1:9/api/personas/impression-requests/di%3Aa%20b"));
    CHECK(!lookup.status("missing").accepted);
    expect_failure([&] { lookup.status("wrong"); }, "protocol", false);
    expect_failure([&] { lookup.status("broken"); }, "protocol", false);
    expect_failure([&] { lookup.submit("broken", "t"); }, "protocol", true);
}

TEST_CASE("request client over a fake HTTP server: response size limit and redirect not followed") {
    auto router = std::make_shared<http::Router>();
    std::atomic<int> requests{0};
    router->get("/health", [&](http::Request&) {
        ++requests;
        return http::Response::json(Json{{"ok", true}});
    });
    router->post("/api/personas/impression-requests", [&](http::Request& c) {
        ++requests;
        const auto body = c.json();
        if (body && json_string(*body, "theme") == "big")
            return http::Response::json(Json{{"pad", std::string(kVoiceRequestMaxResponseBytes, 'x')}});
        if (body && json_string(*body, "theme") == "redirect") return http::Response::redirect("http://127.0.0.1:1/elsewhere", 307);
        return http::Response::json(Json{{"data", {{"request", {{"requestId", json_string(*body, "requestId")}, {"status", "completed"}}}}}});
    });
    http::ServerOptions so;
    so.port = 0;
    http::Server server(so, router);
    server.start();
    const std::string base = "http://127.0.0.1:" + std::to_string(server.bound_port());
    int tokens = 0;
    auto client = create_voice_request_client([&] { return base; }, token_counter(&tokens));
    CHECK(client.submit("di:x", "ok").accepted);
    expect_failure([&] { client.submit("di:x", "big"); }, "down", true);  // 上限超過は通信の失敗 (送信は済んだかもしれない)
    expect_failure([&] { client.submit("di:x", "redirect"); }, "protocol", false);
    server.stop();
}

TEST_CASE("preparation: no resend on unknown, explicit choice, notices (voice-preparation.test.ts + golden)") {
    w2h::TempFlowDb tmp;
    int submissions = 0;
    VoiceRequestPort request;
    request.submit = [&](const std::string&, const std::string&) -> VoiceRequestReceipt {
        ++submissions;
        throw std::runtime_error("timeout after send");
    };
    request.status = [](const std::string& id) { return VoiceRequestReceipt{true, id}; };
    PrepareVoiceChoiceArgs input{"s1", "題", 0, false, request};
    auto state = prepare_voice_choice(input);
    CHECK_EQ(state.requestStatus, std::string("unknown"));
    CHECK_EQ(*state.failure, std::string("protocol"));
    prepare_voice_choice(input);
    CHECK_EQ(submissions, 1);
    state.choice = "wait";
    save_voice_preparation("s1", state);
    CHECK_EQ(read_voice_preparation("s1")->choice, std::string("wait"));
    CHECK(needs_voice_choice(state));
    const auto checked = check_voice_receipt("s1", request);
    CHECK_EQ(checked->requestStatus, std::string("accepted"));
    CHECK(!checked->failure);
    state.choice = "continue";
    CHECK(!needs_voice_choice(state));
    CHECK(!needs_voice_choice(std::nullopt));

    auto prior_args = input;
    prior_args.sessionId = "prior";
    prior_args.priorKnowledge = true;
    const auto prior = prepare_voice_choice(prior_args);
    CHECK_EQ(prior.requestStatus, std::string("not_needed"));
    CHECK_EQ(prior.referenceCount, 0.0);

    auto offline_args = input;
    offline_args.sessionId = "offline";
    offline_args.request->submit = [](const std::string&, const std::string&) -> VoiceRequestReceipt { throw VoiceRequestFailure("down"); };
    const auto offline = prepare_voice_choice(offline_args);
    CHECK_EQ(offline.requestStatus, std::string("unavailable"));
    CHECK(voice_preparation_notice(offline).find("停止中") != std::string::npos);
    CHECK(voice_preparation_notice(offline).find("外部の声なしで開始") != std::string::npos);

    auto none_args = input;
    none_args.sessionId = "none";
    none_args.request.reset();
    CHECK_EQ(prepare_voice_choice(none_args).requestStatus, std::string("unavailable"));
    CHECK_EQ(json_stringify(to_json(*read_voice_preparation("none"))),
             std::string(R"({"referenceCount":0,"priorKnowledge":false,"requestId":"di:none","choice":"unselected","requestStatus":"unavailable"})"));

    auto accepted_args = input;
    accepted_args.sessionId = "acc";
    accepted_args.request->submit = [](const std::string& id, const std::string&) { return VoiceRequestReceipt{false, id + "-v"}; };
    const auto acc = prepare_voice_choice(accepted_args);
    CHECK_EQ(acc.requestStatus, std::string("submitted"));
    CHECK_EQ(acc.requestId, std::string("di:acc-v"));

    // 照会の失敗は failure を記録して状態は保つ
    VoiceRequestPort failing;
    failing.status = [](const std::string&) -> VoiceRequestReceipt { throw VoiceRequestFailure("auth"); };
    CHECK_EQ(*check_voice_receipt("acc", failing)->failure, std::string("auth"));
    CHECK(!check_voice_receipt("missing", failing));

    for (const auto& c : w2h::fx()["voiceNotices"])
        CHECK_EQ(voice_preparation_notice(voice_preparation_from_json(c["state"])), c["notice"].get<std::string>());
}

TEST_CASE("boundary: broken / huge stored payloads are refused") {
    w2h::TempFlowDb tmp;
    auto db = flow::db::get_flow_db();
    db->run("INSERT INTO flow_voice_preparation (session_id, payload, updated_at) VALUES ('b', '{broken', 0)");
    CHECK_THROWS(read_voice_preparation("b"));
    db->run("INSERT INTO flow_voice_preparation (session_id, payload, updated_at) VALUES ('h', ?, 0)",
            {"{\"p\":\"" + std::string(kVoicePreparationMaxBytes, 'x') + "\"}"});
    CHECK_THROWS(read_voice_preparation("h"));
    db->run("INSERT INTO flow_voice_preparation (session_id, payload, updated_at) VALUES ('ok', ?, 0)",
            {"{\"requestStatus\":\"accepted\",\"choice\":\"wait\",\"extra\":1}"});
    CHECK_EQ(read_voice_preparation("ok")->requestStatus, std::string("accepted"));
}

DI_TEST_MAIN()
