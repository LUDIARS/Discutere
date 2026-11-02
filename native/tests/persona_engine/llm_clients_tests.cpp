// tests/persona-engine/local-openai.test.ts の移植 (偽の HTTP サーバ) + codex-cli (偽の codex) + mock。
#include "check.hpp"

#include <atomic>
#include <filesystem>

#include "base/crypto.hpp"
#include "base/env.hpp"
#include "base/fsutil.hpp"
#include "infra/http_server.hpp"
#include "persona_engine/llm/codex_cli.hpp"
#include "persona_engine/llm/local_openai.hpp"
#include "persona_engine/llm/mock.hpp"
#ifndef _WIN32
#include <sys/stat.h>
#endif

using namespace di;
using namespace di::persona_engine;

namespace {
struct FakeServer {
    std::shared_ptr<http::Router> router = std::make_shared<http::Router>();
    std::unique_ptr<http::Server> server;
    std::string base() const { return "http://127.0.0.1:" + std::to_string(server->bound_port()); }
    void start() {
        http::ServerOptions o;
        o.port = 0;
        server = std::make_unique<http::Server>(o, router);
        server->start();
    }
    ~FakeServer() {
        if (server) server->stop();
    }
};
} // namespace

TEST_CASE("local-openai: request shape, trailing slash, content trim, usage map (fake server)") {
    FakeServer fake;
    std::string seen_path, seen_auth;
    Json seen_body;
    fake.router->post("/v1/chat/completions", [&](http::Request& req) {
        seen_path = req.path;
        seen_auth = req.header("authorization").value_or("");
        seen_body = req.json().value_or(Json());
        return http::Response::json(Json{{"choices", Json::array({Json{{"message", Json{{"content", "  やあ、 描野です。  "}}}}})},
                                         {"usage", Json{{"prompt_tokens", 12}, {"completion_tokens", 8}}}});
    });
    fake.start();
    LocalOpenAiClient client({fake.base() + "/v1//", std::string("gemma3:12b"), std::string("test-not-a-real-key"), 5000, {}});
    LLMInvokeArgs args;
    args.prompt = "自己紹介して";
    args.system = "あなたはスケッチ屋。";
    auto res = client.invoke(args);
    CHECK(res.ok);
    CHECK_EQ(res.text, std::string("やあ、 描野です。"));
    CHECK_EQ(*res.usage->input_tokens, 12.0);
    CHECK_EQ(*res.usage->output_tokens, 8.0);
    CHECK_EQ(seen_path, std::string("/v1/chat/completions"));
    CHECK_EQ(seen_auth, std::string("Bearer test-not-a-real-key"));
    CHECK_EQ(json_stringify(seen_body),
             std::string(R"({"model":"gemma3:12b","max_tokens":4096,"messages":[{"role":"system","content":"あなたはスケッチ屋。"},{"role":"user","content":"自己紹介して"}],"stream":false})"));
}

TEST_CASE("local-openai: HTTP error, empty content, broken JSON, oversized response, missing config") {
    FakeServer fake;
    fake.router->post("/e404/chat/completions", [](http::Request&) { return http::Response::json(Json{{"error", "model not found"}}, 404); });
    fake.router->post("/empty/chat/completions", [](http::Request&) { return http::Response::json(Json{{"choices", Json::array()}}); });
    fake.router->post("/broken/chat/completions", [](http::Request&) { return http::Response::text("{not json"); });
    fake.router->post("/huge/chat/completions",
                      [](http::Request&) { return http::Response::text(std::string(LOCAL_OPENAI_MAX_RESPONSE_BYTES + 1, ' ')); });
    fake.router->post("/exact/chat/completions", [](http::Request&) {
        std::string body = R"({"choices":[{"message":{"content":"ok"}}],"pad":")";
        body += std::string(LOCAL_OPENAI_MAX_RESPONSE_BYTES - body.size() - 2, 'p');
        body += "\"}";
        return http::Response::bytes(body, "application/json");
    });
    fake.start();
    LLMInvokeArgs args;
    args.prompt = "x";
    auto r404 = LocalOpenAiClient({fake.base() + "/e404", std::string("m"), std::nullopt, 5000, {}}).invoke(args);
    CHECK(!r404.ok);
    CHECK(r404.error.find("404") != std::string::npos);
    CHECK(r404.error.find("model not found") != std::string::npos);
    CHECK_EQ(LocalOpenAiClient({fake.base() + "/empty", std::string("m"), std::nullopt, 5000, {}}).invoke(args).error,
             std::string("local LLM empty text response"));
    CHECK(!LocalOpenAiClient({fake.base() + "/broken", std::string("m"), std::nullopt, 5000, {}}).invoke(args).ok);
    CHECK(!LocalOpenAiClient({fake.base() + "/huge", std::string("m"), std::nullopt, 5000, {}}).invoke(args).ok);
    auto exact = LocalOpenAiClient({fake.base() + "/exact", std::string("m"), std::nullopt, 5000, {}}).invoke(args);
    CHECK(exact.ok);
    CHECK(!LocalOpenAiClient({"", std::string("m"), std::nullopt, std::nullopt, {}}).invoke(args).ok);
    CHECK(!LocalOpenAiClient({"http://x/v1", std::string(""), std::nullopt, std::nullopt, {}}).invoke(args).ok);
    // 繋がらない (閉じたポート) は transport エラーで ok:false。
    auto closed = LocalOpenAiClient({"http://127.0.0.1:1/v1", std::string("m"), std::nullopt, 2000, {}}).invoke(args);
    CHECK(!closed.ok);
    CHECK(closed.error.rfind("local LLM invoke failed: ", 0) == 0);
}

TEST_CASE("local-openai: fetchImpl injection sees allow_private_network and limits") {
    http::ClientRequest seen;
    LocalOpenAiClient client({"http://localhost:11434/v1/", std::string("m"), std::nullopt, 1234, [&](const http::ClientRequest& r) {
                                  seen = r;
                                  http::ClientResponse res;
                                  res.transport_ok = true;
                                  res.status = 200;
                                  res.body = R"({"choices":[{"message":{"content":"hi"}}]})";
                                  return res;
                              }});
    LLMInvokeArgs args;
    args.prompt = "p";
    auto res = client.invoke(args);
    CHECK(res.ok);
    CHECK(!res.usage);
    CHECK_EQ(seen.url, std::string("http://localhost:11434/v1/chat/completions"));
    CHECK(seen.allow_private_network);
    CHECK_EQ(seen.max_response_bytes, LOCAL_OPENAI_MAX_RESPONSE_BYTES);
    CHECK_EQ(seen.timeout.count(), 1234LL);
    CHECK(!seen.headers.has("authorization"));
}

TEST_CASE("codex-cli: env allowlist, args, conversationOnly and model validation") {
    std::map<std::string, std::string> src = {{"PATH", "/bin"}, {"HOME", "/h"}, {"OPENAI_API_KEY", "x"}, {"DISCUTERE_SECRET", "y"}, {"CODEX_HOME", "/c"}};
    auto env = build_codex_environment(src);
    CHECK_EQ(env.size(), std::size_t(3));
    CHECK(env.find("OPENAI_API_KEY") == env.end());
    auto args = codex_cli_args("gpt-6-sol", std::string("high"), "/tmp/o.txt");
    std::vector<std::string> expected = {"exec", "-m", "gpt-6-sol", "-c", "model_reasoning_effort=\"high\"", "-s", "read-only", "--ephemeral",
                                         "--skip-git-repo-check", "--color", "never", "-o", "/tmp/o.txt", "-"};
    CHECK(args == expected);
    CodexCliClient client({std::string("/nonexistent/codex"), std::string("gpt-6-sol"), 5000, std::nullopt});
    LLMInvokeArgs a;
    a.prompt = "p";
    a.conversationOnly = true;
    CHECK_EQ(client.invoke(a).error, std::string("Codex CLI conversation-only capability is not configured"));
    a.conversationOnly = false;
    a.model = "gpt-6-sol@ultra";  // 未対応の effort の検査 (起動前に拒否)
    CHECK(client.invoke(a).error.rfind("codex-cli: ", 0) == 0);
    a.model = "claude-opus-5";  // one-shot が GPT 以外を拒否 (TS の spawnOneShot が投げるのと同じ)
    CHECK_EQ(client.invoke(a).error, std::string("codex cli spawn failed"));
    a.model = "gpt-6-sol";
    CHECK_EQ(client.invoke(a).error, std::string("codex cli error (ENOENT)"));
}

#ifndef _WIN32
namespace {
std::string fake_codex(const std::string& body) {
    const auto dir = std::filesystem::temp_directory_path() / ("di-fake-codex-" + crypto::uuid_v4());
    fs::ensure_dir(fs::from_path(dir));
    const std::string path = fs::from_path(dir / "codex");
    fs::write_file_atomic(path, "#!/bin/sh\n" + body);
    ::chmod(path.c_str(), 0755);
    return path;
}
} // namespace

TEST_CASE("codex-cli: fake codex writes -o file from stdin (argv, no shell)") {
    // 偽の codex: 最後から 2 番目の引数 (-o の値) に stdin を書く。env に API キーが無いことも確かめる。
    const auto path = fake_codex(
        "for a; do prev2=$prev; prev=$a; done\n"
        "out=\"\"; while [ $# -gt 0 ]; do if [ \"$1\" = \"-o\" ]; then out=$2; fi; shift; done\n"
        "if [ -n \"$OPENAI_API_KEY\" ]; then echo leaked > \"$out\"; exit 0; fi\n"
        "cat > \"$out\"\n");
    di::env::set_for_test("OPENAI_API_KEY", std::string_view("test-not-real"));
    CodexCliClient client({path, std::string("gpt-6-sol@low"), 10'000, std::nullopt});
    LLMInvokeArgs a;
    a.prompt = "hello; rm -rf /";
    a.system = "sys";
    auto r = client.invoke(a);
    di::env::set_for_test("OPENAI_API_KEY", std::nullopt);
    CHECK(r.ok);
    CHECK_EQ(r.text, std::string("[system]\nsys\n\n[user]\nhello; rm -rf /"));
}

TEST_CASE("codex-cli: exit code, empty response, oversized response, timeout") {
    LLMInvokeArgs a;
    a.prompt = "p";
    CHECK_EQ(CodexCliClient({fake_codex("exit 3\n"), std::string("gpt-6-sol"), 10'000, std::nullopt}).invoke(a).error,
             std::string("codex cli exit 3"));
    CHECK_EQ(CodexCliClient({fake_codex("cat >/dev/null\nexit 0\n"), std::string("gpt-6-sol"), 10'000, std::nullopt}).invoke(a).error,
             std::string("codex cli: empty response"));
    // 子の env は許可リストだけなので、書く大きさはスクリプトに埋め込む。
    auto write_n = [](std::size_t n) {
        return "out=\"\"; while [ $# -gt 0 ]; do if [ \"$1\" = \"-o\" ]; then out=$2; fi; shift; done\n"
               "head -c " + std::to_string(n) + " /dev/zero | tr '\\0' 'a' > \"$out\"\n";
    };
    auto exact = CodexCliClient({fake_codex(write_n(CODEX_MAX_RESPONSE_BYTES)), std::string("gpt-6-sol"), 10'000, std::nullopt}).invoke(a);
    CHECK(exact.ok);
    CHECK_EQ(exact.text.size(), CODEX_MAX_RESPONSE_BYTES);
    auto over = CodexCliClient({fake_codex(write_n(CODEX_MAX_RESPONSE_BYTES + 1)), std::string("gpt-6-sol"), 10'000, std::nullopt}).invoke(a);
    CHECK_EQ(over.error, std::string("codex cli: response exceeds 1048576 bytes"));
    auto slow = CodexCliClient({fake_codex("sleep 5\n"), std::string("gpt-6-sol"), 300, std::nullopt}).invoke(a);
    CHECK_EQ(slow.error, std::string("codex cli timeout (300ms)"));
}
#endif

TEST_CASE("mock: scripted responders then fallback, counts calls, keeps last invocation") {
    MockLLMClient llm({[](const LLMInvokeArgs&) -> std::variant<std::string, LLMResult> { return std::string(R"({"action":"skip","reasoning":"first"})"); },
                       [](const LLMInvokeArgs& a) -> std::variant<std::string, LLMResult> { return LLMResult::failure("err:" + a.prompt); }});
    LLMInvokeArgs a;
    a.prompt = "p1";
    CHECK_EQ(llm.invoke(a).text, std::string(R"({"action":"skip","reasoning":"first"})"));
    a.prompt = "p2";
    CHECK_EQ(llm.invoke(a).error, std::string("err:p2"));
    auto third = llm.invoke(a);
    CHECK(third.ok);
    CHECK_EQ(third.text, std::string(R"({"action":"skip","reasoning":"mock exhausted"})"));
    CHECK_EQ(llm.calls(), 3);
    CHECK_EQ(llm.last_invocation()->prompt, std::string("p2"));
}

DI_TEST_MAIN()
