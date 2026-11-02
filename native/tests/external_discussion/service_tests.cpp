// external-discussion/service.ts と api/external-discussion-routes.ts の移植の検証:
// tests/flow/external-discussion.test.ts の観点 + Node golden (LLM に渡す system / prompt・結果) + 本文上限の境界。
#include "check.hpp"

#include <atomic>
#include <future>
#include <thread>

#include <nlohmann/json.hpp>

#include "api/external_discussion_routes.hpp"
#include "base/crypto.hpp"
#include "base/strings.hpp"
#include "external_discussion/participation.hpp"
#include "external_discussion/service.hpp"
#include "infra/http_client.hpp"
#include "infra/http_server.hpp"
#include "persona_engine/llm/mock.hpp"
#include "w2i_support.hpp"

using namespace di;
using namespace di::external_discussion;
using persona_engine::LLMInvokeArgs;
using persona_engine::LLMResult;

namespace {

class FnLLM : public persona_engine::LLMClient {
public:
    std::function<LLMResult(const LLMInvokeArgs&)> fn;
    std::vector<LLMInvokeArgs> calls;
    std::mutex m;
    LLMResult invoke(const LLMInvokeArgs& a) override {
        {
            std::lock_guard lock(m);
            calls.push_back(a);
        }
        return fn(a);
    }
};

nlohmann::json plain(const Json& j) { return nlohmann::json::parse(j.dump()); }

ConversationInput conv(const Json& messages, const std::string& scene = "actio:team:thread") {
    return parse_conversation(Json{{"scene", scene}, {"enabled", true}, {"messages", messages}});
}

} // namespace

TEST_CASE("service: Node golden (system / prompt / 結果 / 保存した行)") {
    for (const auto& c : w2i::load_fixture("external_service.json")) {
        const std::string name = c["name"].get<std::string>();
        auto db = w2i::temp_db("ext");
        auto store = std::make_shared<ParticipationStore>(db);
        auto llm = std::make_shared<FnLLM>();
        const bool fail = c["fail"].get<bool>();
        const std::string text = c["llmText"].get<std::string>();
        llm->fn = [&](const LLMInvokeArgs&) { return fail ? LLMResult::failure("boom") : LLMResult::success(text); };
        std::vector<double> seq;
        for (const auto& v : c["rngSeq"]) seq.push_back(v.get<double>());
        std::size_t i = 0;
        ExternalDiscussionService svc(store, llm, "configured-model", [] { return std::int64_t(1'000'000); },
                                      [&] { return seq[std::min(i++, seq.size() - 1)]; });
        std::optional<ParticipationResult> result;
        std::string error;
        try {
            result = svc.consider(conv(c["messages"]));
        } catch (const std::exception& e) {
            error = e.what();
        }
        CHECK_EQ(llm->calls.size(), c["calls"].size());
        for (std::size_t k = 0; k < llm->calls.size(); ++k) {
            const auto& a = llm->calls[k];
            const auto& e = c["calls"][k];
            CHECK_EQ(*a.model, e["model"].get<std::string>());
            CHECK_EQ(*a.maxTokens, e["maxTokens"].get<int>());
            CHECK_EQ(*a.timeoutMs, e["timeoutMs"].get<long long>());
            CHECK(a.conversationOnly);
            CHECK_EQ(*a.system, e["system"].get<std::string>());
            CHECK_EQ(a.prompt, e["prompt"].get<std::string>());
        }
        if (c["result"].is_null()) {
            CHECK(!result);
            // 例外の文言: Node の SyntaxError 以外は同じ
            if (name != "not json") CHECK_EQ(error, c["error"].get<std::string>());
        } else {
            CHECK(result.has_value());
            if (plain(to_json(*result)) != plain(c["result"])) throw test::Failure(name + ": " + to_json(*result).dump());
        }
        Json rows = Json::array();
        for (const auto& row : db->all("SELECT id, state, result FROM external_discussion_attempts")) {
            Json r = Json::object();
            r["id"] = row.str("id");
            r["state"] = row.str("state");
            r["result"] = row["result"].is_null() ? Json(nullptr) : Json(row.str("result"));
            rows.push_back(r);
        }
        if (plain(rows) != plain(c["rows"])) throw test::Failure(name + ": rows " + rows.dump());
    }
}

TEST_CASE("service: external-discussion.test.ts (再生成しない・disabled・抽選は 1 回)") {
    auto db = w2i::temp_db("ext2");
    std::int64_t now = 1'000'000;
    auto llm = std::make_shared<FnLLM>();
    llm->fn = [](const LLMInvokeArgs&) { return LLMResult::success(R"({"text":"段階的に試すと影響を確認できそうです。"})"); };
    ExternalDiscussionService svc(std::make_shared<ParticipationStore>(db), llm, "configured-model", [&] { return now; }, [] { return 0.0; });
    auto input = conv(Json::array({Json{{"id", "1"}, {"kind", "human"}, {"text", "どう進めよう"}, {"at", now - 179'999}}}));
    CHECK(!opportunity(input, now));
    ++now;
    const auto first = svc.consider(input);
    CHECK_EQ(first.status, std::string("proposal"));
    CHECK(svc.consider(input) == first);
    CHECK_EQ(llm->calls.size(), std::size_t(1));
    input.enabled = false;
    CHECK_EQ(svc.consider(input).status, std::string("disabled"));

    auto never = std::make_shared<FnLLM>();
    never->fn = [](const LLMInvokeArgs&) -> LLMResult { throw std::runtime_error("should not generate"); };
    ExternalDiscussionService skip(std::make_shared<ParticipationStore>(db), never, "model", [&] { return now; }, [] { return 0.99; });
    Json msgs = Json::array();
    for (int i = 0; i < 3; ++i) msgs.push_back(Json{{"id", std::to_string(i)}, {"kind", "human"}, {"text", "検討中"}, {"at", now - 1000 + i}});
    auto active = conv(msgs, "actio:another:thread");
    CHECK_EQ(skip.consider(active).status, std::string("skipped"));
    CHECK_EQ(skip.consider(active).status, std::string("skipped"));
    CHECK(never->calls.empty());
}

TEST_CASE("routes: 503 / 401 (定数時間比較) / 400 / 429 / 503") {
    auto call = [](http::Router& r, const std::string& body, std::optional<std::string> auth) {
        http::Request q;
        q.method = "POST";
        q.path = "/internal/external-discussion/consider";
        q.remote_address = "127.0.0.1";
        q.body = body;
        if (auth) q.headers.set("Authorization", *auth);
        return r.handle(q);
    };
    const std::string valid = R"({"scene":"actio:team:thread","enabled":true,"messages":[{"id":"1","kind":"human","text":"x","at":1000}]})";
    {
        http::Router r;
        r.mount("/internal/external-discussion", api::external_discussion_routes(nullptr, "secret"));
        CHECK_EQ(call(r, valid, std::string("Bearer secret")).status, 503);
        CHECK_EQ(call(r, valid, std::string("Bearer secret")).body, std::string(R"({"error":"external discussion unavailable"})"));
    }
    auto db = w2i::temp_db("routes");
    auto llm = std::make_shared<FnLLM>();
    std::promise<void> gate;
    auto gate_future = gate.get_future().share();
    std::atomic<int> entered{0};
    llm->fn = [&](const LLMInvokeArgs&) {
        ++entered;
        gate_future.wait();
        return LLMResult::success(R"({"text":"案"})");
    };
    auto svc = std::make_shared<ExternalDiscussionService>(std::make_shared<ParticipationStore>(db), llm, "m",
                                                           [] { return std::int64_t(1'000'000); }, [] { return 0.0; });
    {
        http::Router r;
        r.mount("/internal/external-discussion", api::external_discussion_routes(svc, ""));
        CHECK_EQ(call(r, valid, std::string("Bearer ")).status, 503);  // 秘密が空なら使えない
    }
    http::Router r;
    r.mount("/internal/external-discussion", api::external_discussion_routes(svc, "secret"));
    CHECK_EQ(call(r, valid, std::nullopt).status, 401);
    CHECK_EQ(call(r, valid, std::string("Bearer secreT")).status, 401);
    CHECK_EQ(call(r, valid, std::string("Bearer secret2")).status, 401);
    CHECK_EQ(call(r, valid, std::string("bearer secret")).status, 401);
    CHECK_EQ(call(r, "{", std::string("Bearer secret")).status, 400);
    CHECK_EQ(call(r, "{", std::string("Bearer secret")).body, std::string(R"({"error":"invalid conversation"})"));
    CHECK_EQ(call(r, R"({"scene":"bad","enabled":true,"messages":[]})", std::string("Bearer secret")).status, 400);

    // 同時 4 本まで。5 本目は 429
    std::vector<std::future<int>> running;
    for (int i = 0; i < 4; ++i) {
        const std::string body = R"({"scene":"actio:s)" + std::to_string(i) + R"(:t","enabled":true,"messages":[{"id":"1","kind":"human","text":"x","at":1000}]})";
        running.push_back(std::async(std::launch::async, [&, body] { return call(r, body, std::string("Bearer secret")).status; }));
    }
    for (int i = 0; i < 300 && entered.load() < 4; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK_EQ(entered.load(), 4);
    const auto busy = call(r, R"({"scene":"actio:s9:t","enabled":true,"messages":[{"id":"1","kind":"human","text":"x","at":1000}]})",
                           std::string("Bearer secret"));
    CHECK_EQ(busy.status, 429);
    CHECK_EQ(busy.body, std::string(R"({"error":"discussion busy"})"));
    gate.set_value();
    for (auto& f : running) CHECK_EQ(f.get(), 200);

    // 生成の失敗は 503
    llm->fn = [](const LLMInvokeArgs&) { return LLMResult::failure("x"); };
    const auto failed = call(r, R"({"scene":"actio:fail:t","enabled":true,"messages":[{"id":"1","kind":"human","text":"x","at":1000}]})",
                             std::string("Bearer secret"));
    CHECK_EQ(failed.status, 503);
    CHECK_EQ(failed.body, std::string(R"({"error":"discussion generation failed"})"));
}

TEST_CASE("routes: 本文上限 160,000 バイト (ちょうど / +1 / 巨大) を実サーバで") {
    auto router = std::make_shared<http::Router>();
    router->mount("/internal/external-discussion", api::external_discussion_routes(nullptr, "secret"));
    CHECK_EQ(router->max_body_for("POST", "/internal/external-discussion/consider"), api::kExternalDiscussionMaxBody);
    http::ServerOptions o;
    o.port = 0;
    http::Server server(o, router);
    server.start();
    auto send = [&](std::size_t n) {
        http::ClientRequest q;
        q.method = "POST";
        q.url = "http://127.0.0.1:" + std::to_string(server.bound_port()) + "/internal/external-discussion/consider";
        q.headers.set("Authorization", "Bearer secret");
        q.body = std::string(n, ' ');
        q.allow_private_network = true;
        return http::fetch(q).status;
    };
    CHECK_EQ(send(160'000), 503);  // 上限ちょうどは通る (service が無いので 503)
    CHECK_EQ(send(160'001), 413);
    // 巨大な本文はサーバが 413 を返して接続を閉じる (送り切る前に切られると通信失敗 = 0 になる)
    const int huge = send(4u << 20);
    CHECK(huge == 413 || huge == 0);
    server.stop();
}

DI_TEST_MAIN()
