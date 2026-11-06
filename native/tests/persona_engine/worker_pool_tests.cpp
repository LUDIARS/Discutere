// worker_pool のテスト (1/2)。準備は worker_pool_support.hpp。
#include "worker_pool_support.hpp"

using namespace worker_pool_support;

TEST_CASE("pool: 起動・一覧・register の settle・停止") {
    const auto dir = w2i::temp_dir("pool");
    auto sp = std::make_shared<Spawns>();
    std::vector<std::string> warns;
    auto pool = make_pool(dir, sp, &warns);
    CHECK(!pool->has_worker("con-opus"));
    CHECK(!pool->start_worker("ghost"));
    CHECK(pool->start_worker("con-opus"));
    CHECK(pool->start_worker("con-opus"));  // 起動済はスキップ
    CHECK_EQ(sp->spawned.size(), std::size_t(1));
    CHECK(fs::is_file(fs::join(dir, "prompts/con-opus.md")));
    CHECK(fs::is_dir(fs::join(dir, "turns")));  // 相対は cwd から
    pool->start();
    CHECK_EQ(sp->spawned.size(), std::size_t(2));

    auto listed = pool->list_configured();
    CHECK_EQ(listed.size(), std::size_t(2));
    CHECK(listed[0].running && !listed[0].registered && !listed[0].settling);
    CHECK_EQ(to_json(listed[0]).dump(),
             std::string(R"({"id":"con-opus","role":"否定派","provider":"claude","model":"claude-opus-4","running":true,"registered":false,"settling":false,"busy":false,"pid":1000,"port":null})"));

    CHECK(!pool->register_port("ghost", 1234));
    CHECK_EQ(warns.back(), std::string("register from unknown worker ghost"));
    CHECK(pool->register_port("con-opus", 4321, 77));
    CHECK(pool->list_configured()[0].settling);
    CHECK(!pool->is_ready("con-opus"));
    wait_ready(*pool, "con-opus");
    CHECK(pool->is_ready("con-opus"));
    CHECK(pool->list_configured()[0].registered);
    CHECK_EQ(to_json(pool->snapshot()[0]).dump(),
             std::string(R"({"id":"con-opus","provider":"claude","model":"claude-opus-4","port":4321,"settling":false,"busy":false,"pid":1000,"lictorPid":77})"));

    // 再 register は settle をやり直す / 停止したワーカーの古いタイマーは効かない
    CHECK(pool->register_port("pro-gpt", 5555));
    CHECK(pool->stop_worker("pro-gpt"));
    CHECK(!pool->stop_worker("pro-gpt"));
    std::this_thread::sleep_for(80ms);
    CHECK(!pool->is_ready("pro-gpt"));
    CHECK(pool->get_worker_config("pro-gpt").has_value());
    CHECK(!pool->get_worker_config("ghost"));

    pool->stop();
    CHECK(!pool->has_worker("con-opus"));
    CHECK_EQ(sp->killed.size(), std::size_t(2));
    CHECK_EQ(*sp->killed.back().second, std::int64_t(77));
}

TEST_CASE("pool: dispatch (注入 2 段・ターン JSON・callback・busy・期限・/v1/keys 失敗・stop)") {
    const auto dir = w2i::temp_dir("dispatch");
    FakeLictor lictor;
    auto sp = std::make_shared<Spawns>();
    std::vector<std::string> warns;
    auto pool = make_pool(dir, sp, &warns);
    pool->start();

    CHECK_THROWS(pool->dispatch("ghost", {"r1", "s", "p"}));
    try {
        pool->dispatch("con-opus", {"r1", "s", "p"});
        CHECK(false);
    } catch (const std::runtime_error& e) {
        CHECK_EQ(std::string(e.what()), std::string("worker con-opus not registered (no port)"));
    }
    pool->register_port("con-opus", lictor.port());
    try {
        pool->dispatch("con-opus", {"r1", "s", "p"});
        CHECK(false);
    } catch (const std::runtime_error& e) {
        CHECK(str::contains(e.what(), "still settling"));
    }
    wait_ready(*pool, "con-opus");
    CHECK_THROWS(pool->dispatch("con-opus", {"../evil", "s", "p"}));  // ファイル名に使う reqId は安全な文字だけ

    TokenUsage usage;
    usage.input_tokens = 10;
    lictor.on_submit = [pool, usage](const std::string& rid) { pool->on_utterance(rid, "con-opus", "発話です", usage); };
    const auto res = pool->dispatch("con-opus", {"req-1", "SYS", "PROMPT"});
    CHECK_EQ(res.text, std::string("発話です"));
    CHECK_EQ(*res.usage->input_tokens, 10.0);
    {
        std::lock_guard lock(lictor.mutex);
        CHECK_EQ(lictor.keys.size(), std::size_t(2));
        CHECK(str::starts_with(lictor.keys[0], "[TURN] req-1 "));
        CHECK(str::ends_with(lictor.keys[0], "/turns/req-1.json"));
        CHECK_EQ(lictor.keys[1], std::string("\r"));
    }
    const auto turn = fs::read_file(fs::join(dir, "turns/req-1.json"));
    CHECK_EQ(*turn, std::string("{\n  \"reqId\": \"req-1\",\n  \"workerId\": \"con-opus\",\n  \"system\": \"SYS\",\n  \"prompt\": \"PROMPT\"\n}"));
    CHECK(!pool->list_configured()[0].busy);

    // busy: 走行中の 2 本目は拒否
    lictor.on_submit = [](const std::string&) {};
    auto first = std::async(std::launch::async, [&] {
        try {
            pool->dispatch("con-opus", {"req-2", "", ""});
            return std::string("ok");
        } catch (const std::exception& e) {
            return std::string(e.what());
        }
    });
    std::this_thread::sleep_for(300ms);
    try {
        pool->dispatch("con-opus", {"req-3", "", ""});
        CHECK(false);
    } catch (const std::runtime_error& e) {
        CHECK_EQ(std::string(e.what()), std::string("worker con-opus busy"));
    }
    // 期限: callback が来なければ turn timeout、busy は戻る
    CHECK_EQ(first.get(), std::string("worker con-opus turn timeout (1500ms)"));
    CHECK(!pool->list_configured()[0].busy);
    pool->on_utterance("req-2", "con-opus", "late");  // 期限切れの callback は warn だけ
    CHECK_EQ(warns.back(), std::string("utterance for unknown/expired reqId req-2"));

    // /v1/keys が失敗したら本文 120 文字までを含むエラー、busy は戻る
    lictor.status = 500;
    try {
        pool->dispatch("con-opus", {"req-4", "", ""});
        CHECK(false);
    } catch (const std::runtime_error& e) {
        CHECK_EQ(std::string(e.what()), "/v1/keys 500: " + std::string(120, 'e'));
    }
    CHECK(!pool->list_configured()[0].busy);
    lictor.status = 200;

    // stop: 待っているターンは "pool stopped"
    auto waiting = std::async(std::launch::async, [&] {
        try {
            pool->dispatch("con-opus", {"req-5", "", ""});
            return std::string("ok");
        } catch (const std::exception& e) {
            return std::string(e.what());
        }
    });
    std::this_thread::sleep_for(300ms);
    pool->stop();
    CHECK_EQ(waiting.get(), std::string("pool stopped"));
}

TEST_CASE("pool: Lictor に繋がらなければ fetch failed") {
    const auto dir = w2i::temp_dir("noconn");
    auto pool = make_pool(dir, std::make_shared<Spawns>());
    pool->start();
    pool->register_port("con-opus", 1);  // 誰も listen していない
    wait_ready(*pool, "con-opus");
    try {
        pool->dispatch("con-opus", {"req-x", "", ""});
        CHECK(false);
    } catch (const std::runtime_error& e) {
        CHECK_EQ(std::string(e.what()), std::string("fetch failed"));
    }
    CHECK(!pool->list_configured()[0].busy);
}

TEST_CASE("client: ルーティング・JSON 包み・fallback の model") {
    const auto dir = w2i::temp_dir("client");
    FakeLictor lictor;
    auto pool = make_pool(dir, std::make_shared<Spawns>());
    WorkerPoolClient bare(pool);
    LLMInvokeArgs a;
    a.prompt = "p";
    a.conversationOnly = true;
    CHECK_EQ(bare.invoke(a).error, std::string("Persistent worker cannot accept external conversation-only requests"));
    a.conversationOnly = false;
    CHECK_EQ(bare.invoke(a).error, std::string("worker-pool: personaId 未指定 (ルーティング不能)"));
    a.personaId = "";
    CHECK_EQ(bare.invoke(a).error, std::string("worker-pool: personaId 未指定 (ルーティング不能)"));
    a.personaId = "con-opus";
    CHECK_EQ(bare.invoke(a).error, std::string("worker-pool: worker 'con-opus' 未起動 かつ fallback 無し"));

    auto fb = std::make_shared<MockLLMClient>(std::vector<MockResponder>{}, "fallback");
    WorkerPoolClient with_fb(pool, fb);
    CHECK_EQ(with_fb.invoke(a).text, std::string("fallback"));
    CHECK_EQ(*fb->last_invocation()->model, std::string("claude-opus-4"));
    a.personaId = "pro-gpt";
    a.model = "claude-x";
    with_fb.invoke(a);
    CHECK(!fb->last_invocation()->model);  // 非 claude モデルは渡さない
    a.personaId = "unknown";
    with_fb.invoke(a);
    CHECK_EQ(*fb->last_invocation()->model, std::string("claude-x"));  // 定義が無ければ args.model
    a.model = "GPT";
    with_fb.invoke(a);
    CHECK(!fb->last_invocation()->model);

    pool->start();
    pool->register_port("con-opus", lictor.port());
    wait_ready(*pool, "con-opus");
    a.personaId = "con-opus";
    a.system = "SYS";
    lictor.on_submit = [pool](const std::string& rid) { pool->on_utterance(rid, "con-opus", "  言いたいこと \n"); };
    auto r = bare.invoke(a);
    CHECK(r.ok);
    CHECK_EQ(r.text, std::string(R"({"action":"post_utterance","text":"言いたいこと"})"));
    lictor.on_submit = [pool](const std::string& rid) { pool->on_utterance(rid, "con-opus", "   "); };
    CHECK_EQ(bare.invoke(a).text, std::string(R"j({"action":"skip","reasoning":"worker skip (空応答)"})j"));
    lictor.status = 500;
    CHECK(str::starts_with(bare.invoke(a).error, "worker-pool: /v1/keys 500"));
    CHECK_EQ(with_fb.invoke(a).text, std::string("fallback"));  // 失敗 → fallback
    pool->stop();
}

DI_TEST_MAIN()
