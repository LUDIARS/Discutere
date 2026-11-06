// 埋め込み (src/core/vectors/embed-retry.ts / embedder.ts)。tests/core/embed-retry.test.ts と
// tests/core/query-embed-cache.test.ts の設定検証を移し、偽の OpenAI 互換サーバで通信を確かめる。
#include "core/test_util.hpp"

#include <atomic>
#include <memory>

#include "core/vectors/embedder.hpp"
#include "infra/http_server.hpp"

using namespace di;
using namespace di::core::vectors;

namespace {
const SleepFn no_sleep = [](double) {};
const WarnFn no_warn = [](const std::string&) {};

EmbeddingConfig valid_config(const std::string& base = "http://127.0.0.1:11434/v1") {
    EmbeddingConfig c;
    c.enabled = true;
    c.baseUrl = base;
    c.model = "test-model";
    c.timeoutMs = 1'000;
    c.batchSize = 2;
    return c;
}

template <class F>
bool throws_with(F&& fn, const std::string& needle) {
    try {
        fn();
    } catch (const std::exception& e) {
        return std::string(e.what()).find(needle) != std::string::npos;
    }
    return false;
}

/** 偽の /v1/embeddings。handler で応答を差し替える。 */
struct FakeServer {
    std::shared_ptr<http::Router> router = std::make_shared<http::Router>();
    std::unique_ptr<http::Server> server;
    std::function<http::Response(http::Request&)> handler;
    std::atomic<int> calls{0};
    std::string last_body;
    std::string last_auth;
    FakeServer() {
        router->post("/v1/embeddings", [this](http::Request& r) {
            ++calls;
            last_body = r.body;
            last_auth = r.header("authorization").value_or("");
            return handler(r);
        }, {8u << 20});
        http::ServerOptions o;
        o.port = 0;
        server = std::make_unique<http::Server>(o, router);
        server->start();
    }
    ~FakeServer() { server->stop(); }
    std::string base() const { return "http://127.0.0.1:" + std::to_string(server->bound_port()) + "/v1/"; }
};

/** 入力の texts をそのまま [len, i] のベクトルにして返す応答 (index は逆順で返す)。 */
http::Response echo_vectors(http::Request& r) {
    const Json body = *r.json();
    Json data = Json::array();
    const auto& input = body["input"];
    for (std::size_t i = input.size(); i-- > 0;)
        data.push_back({{"index", i}, {"embedding", Json::array({static_cast<double>(input[i].get<std::string>().size()), static_cast<double>(i)})}});
    return http::Response::json(Json{{"data", data}});
}
} // namespace

TEST_CASE("isRetryableEmbeddingError classifies transient vs permanent") {
    CHECK(is_retryable_embedding_error(EmbeddingHttpError(500, "Internal")));
    CHECK(is_retryable_embedding_error(EmbeddingHttpError(429, "Too Many")));
    CHECK(!is_retryable_embedding_error(EmbeddingHttpError(400, "Bad Request")));
    CHECK(!is_retryable_embedding_error(EmbeddingHttpError(404, "Not Found")));
    CHECK(is_retryable_embedding_error(EmbeddingTransportError("timeout")));
    CHECK(!is_retryable_embedding_error(std::runtime_error("応答件数不一致")));
    CHECK_EQ(std::string(EmbeddingHttpError(503, "").what()), std::string("embedding HTTP 503"));
}

TEST_CASE("withEmbedRetry: transient recovery, fail-fast, exhaustion, validation") {
    int calls = 0;
    std::vector<std::string> warns;
    auto r = with_embed_retry(
        [&] {
            if (++calls < 3) throw EmbeddingTransportError("timeout");
            return std::string("ok");
        },
        {3, 1}, [&](const std::string& m) { warns.push_back(m); }, no_sleep);
    CHECK_EQ(r, std::string("ok"));
    CHECK_EQ(calls, 3);
    CHECK_EQ(warns.size(), 2u);
    CHECK_EQ(warns[0], std::string("一過性の埋め込み失敗 (timeout) — 1ms 待って再試行 (1/2)"));
    CHECK_EQ(warns[1], std::string("一過性の埋め込み失敗 (timeout) — 2ms 待って再試行 (2/2)"));

    calls = 0;
    CHECK(throws_with([&] { with_embed_retry([&]() -> int { ++calls; throw EmbeddingHttpError(400, "Bad Request"); }, {3, 1}, no_warn, no_sleep); },
                      "HTTP 400"));
    CHECK_EQ(calls, 1);
    calls = 0;
    CHECK(throws_with([&] { with_embed_retry([&]() -> int { ++calls; throw EmbeddingHttpError(503, "Unavailable"); }, {3, 1}, no_warn, no_sleep); },
                      "HTTP 503"));
    CHECK_EQ(calls, 3);
    CHECK(throws_with([&] { with_embed_retry([] { return 1; }, {INFINITY, 1}, no_warn, no_sleep); }, "attempts must be a positive integer"));
    CHECK(throws_with([&] { with_embed_retry([] { return 1; }, {1.5, 1}, no_warn, no_sleep); }, "attempts must be a positive integer"));
    CHECK(throws_with([&] { with_embed_retry([] { return 1; }, {1, NAN}, no_warn, no_sleep); }, "baseDelayMs must be a non-negative finite number"));
}

TEST_CASE("embedder config validation (same messages as TS)") {
    for (double b : {0.0, -1.0, 1.5}) {
        auto c = valid_config();
        c.batchSize = b;
        CHECK(throws_with([&] { create_open_ai_compat_embedder(c); }, "batchSize must be a positive integer"));
    }
    auto c = valid_config();
    c.timeoutMs = 0;
    CHECK(throws_with([&] { create_open_ai_compat_embedder(c); }, "timeoutMs must be a positive number"));
    c = valid_config();
    c.model = "  ";
    CHECK(throws_with([&] { create_open_ai_compat_embedder(c); }, "model must not be empty"));
    CHECK(throws_with([&] { create_open_ai_compat_embedder(valid_config("file:///tmp/embeddings")); }, "HTTP(S)"));
    CHECK(throws_with([&] { create_open_ai_compat_embedder(valid_config("http://u:p@127.0.0.1/v1")); }, "without embedded credentials"));
    CHECK(throws_with([&] { create_open_ai_compat_embedder(valid_config("http://127.0.0.1/v1?x=1")); }, "query string or fragment"));
    CHECK(throws_with([&] { create_open_ai_compat_embedder(valid_config("not a url")); }, "valid HTTP(S) URL"));
    CHECK_EQ(embedding_endpoint(valid_config("http://127.0.0.1:11434/v1///")), std::string("http://127.0.0.1:11434/v1/embeddings"));
}

TEST_CASE("embeds over a fake server in input order, in batches, with Bearer only when set") {
    FakeServer fake;
    fake.handler = echo_vectors;
    auto c = valid_config(fake.base());
    c.apiKey = "test-not-a-real-key";
    auto client = create_open_ai_compat_embedder(c, {no_warn, std::nullopt, no_sleep});
    auto v = client->embed({"a", "bb", "ccc", "dddd", "eeeee"});
    CHECK_EQ(v.size(), 5u);
    for (std::size_t i = 0; i < v.size(); ++i) CHECK_EQ(v[i][0], static_cast<double>(i + 1));
    CHECK_EQ(fake.calls.load(), 3);  // batchSize 2
    CHECK_EQ(fake.last_body, std::string(R"({"model":"test-model","input":["eeeee"]})"));
    CHECK_EQ(fake.last_auth, std::string("Bearer test-not-a-real-key"));
    CHECK(client->embed({}).empty());
}

TEST_CASE("transient 5xx is retried; 4xx and malformed responses fail fast") {
    FakeServer fake;
    int n = 0;
    fake.handler = [&](http::Request& r) {
        if (++n == 1) return http::Response::text("upstream secret text", 503);
        return echo_vectors(r);
    };
    std::vector<std::string> warns;
    auto client = create_open_ai_compat_embedder(valid_config(fake.base()),
                                                 {[&](const std::string& m) { warns.push_back(m); }, std::nullopt, no_sleep});
    CHECK_EQ(client->embed({"x"}).size(), 1u);
    CHECK_EQ(warns.size(), 1u);
    CHECK(warns[0].find("upstream secret") == std::string::npos);  // 応答本文はログに出さない

    fake.handler = [](http::Request&) { return http::Response::text("bad", 400); };
    fake.calls = 0;
    CHECK(throws_with([&] { client->embed({"x"}); }, "embedding HTTP 400 Bad Request"));
    CHECK_EQ(fake.calls.load(), 1);

    const std::vector<std::pair<std::string, std::string>> bad = {
        {R"({"data":[]})", "応答件数不一致: 入力 1 件 / 応答 0 件"},
        {R"({"data":[{"index":3,"embedding":[1]}]})", "index が不正または重複している (index=3)"},
        {R"({"data":[{"index":0,"embedding":[]}]})", "有効なベクトルが無い (index=0)"},
        {R"({"data":[{"index":0,"embedding":[1,"x"]}]})", "有効なベクトルが無い"},
        {R"({"data":{}})", "応答件数不一致"},
        {R"({"nodata":1})", "応答件数不一致"},
        {"not json", "JSON が不正"},
    };
    for (const auto& [body, msg] : bad) {
        fake.handler = [b = body](http::Request&) { return http::Response::bytes(b, "application/json"); };
        fake.calls = 0;
        CHECK(throws_with([&] { client->embed({"x"}); }, msg));
        CHECK_EQ(fake.calls.load(), 1);
    }
    fake.handler = [](http::Request&) {
        return http::Response::json(Json{{"data", Json::array({{{"index", 0}, {"embedding", {1, 2}}}, {{"index", 1}, {"embedding", {1}}}})}});
    };
    CHECK(throws_with([&] { client->embed({"x", "y"}); }, "次元が一致しない"));
    fake.handler = [](http::Request&) {
        return http::Response::json(Json{{"data", Json::array({{{"index", 0}, {"embedding", {1}}}, {{"index", 0}, {"embedding", {1}}}})}});
    };
    CHECK(throws_with([&] { client->embed({"x", "y"}); }, "重複"));
}

TEST_CASE("boundaries: response size limit, redirects and unreachable server") {
    FakeServer fake;
    fake.handler = echo_vectors;
    EmbedderLimits limits;
    limits.max_response_bytes = 64;
    auto client = create_open_ai_compat_embedder(valid_config(fake.base()), {no_warn, EmbedRetryOptions{1, 0}, no_sleep}, limits);
    CHECK_EQ(client->embed({"a"}).size(), 1u);  // 小さい応答は通る
    CHECK_THROWS(client->embed({"a", "b"}));      // 上限を超える応答は読まない
    fake.handler = [](http::Request&) { return http::Response::redirect("http://example.com/", 302); };
    CHECK(throws_with([&] { client->embed({"a"}); }, "redirect"));
    auto c = valid_config("http://127.0.0.1:1/v1");
    auto dead = create_open_ai_compat_embedder(c, {no_warn, EmbedRetryOptions{2, 0}, no_sleep});
    CHECK(throws_with([&] { dead->embed({"a"}); }, "embedding request failed"));
}

DI_TEST_MAIN()
