// adapter のテスト (3/3)。準備は adapter_support.hpp。
#include "adapter_support.hpp"

using namespace adapter_support;

TEST_CASE("event-bridge: evidence の guildId=web は web:<room>、payload の sessionId が最優先、壊れた payload は session 無し") {
    w3n::TempDir dir;
    w3n::TestConfig cfg(dir, false);
    auto core = core::create_core(dir.file("db.kuzu"), dir.file("events.jsonl"));
    StubEngine stub;
    auto bridge = create_event_bridge(*core, stub.bridge(), {ws, 60'000, std::nullopt, {}});
    tick();
    const auto gap_id = make_gap(*core, "web 議題");
    core->raw().run("UPDATE design_gaps SET evidence_json = ? WHERE id = ?", {R"({"guildId":"web","channelId":"room-1"})", gap_id});
    bridge->poll_once();
    CHECK_EQ(core->raw().get("SELECT scene FROM sessions WHERE title = ?", {"discussion-of-gap:" + gap_id})->str("scene"), std::string("web:room-1"));

    tick();
    const double t = static_cast<double>(time::now_ms());
    core->raw().run("INSERT INTO events (id, event_type, payload_json, created_at) VALUES ('c1', 'Custom', ?, ?)",
                    {R"({"sessionId":"s-explicit","id":"x"})", t});
    core->raw().run("INSERT INTO events (id, event_type, payload_json, created_at) VALUES ('c2', 'Custom2', '[1,2]', ?)", {t + 1});
    { std::lock_guard l(stub.m); stub.fired.clear(); }
    // 前回の poll で作った gap session の SessionCreated も今回読む。
    CHECK(bridge->poll_once() >= 2);
    {
        std::lock_guard l(stub.m);
        int seen = 0;
        for (const auto& f : stub.fired) {
            if (f.kind == "Custom") {
                CHECK_EQ(f.sessionId.value_or(""), std::string("s-explicit"));
                seen += 1;
            }
            if (f.kind == "Custom2") {
                CHECK(!f.sessionId);
                CHECK(!f.payload);
                seen += 1;
            }
        }
        CHECK_EQ(seen, 2);
    }
    // 終結イベント: resetSession。personas/rules の無いスタブでは記録できない旨を既定の warn に流す (落ちない)。
    core->raw().run("INSERT INTO events (id, event_type, payload_json, created_at) VALUES ('c3', 'HypothesisRejected', ?, ?)",
                    {R"({"id":"h-x","sessionId":"s-close"})", t + 2});
    CHECK_EQ(bridge->poll_once(), 1);
    CHECK_EQ(stub.resets.back(), std::string("s-close"));
    bridge->stop();
    core->close();
}

TEST_CASE("境界: 1 回の poll は kMaxEventsPerPoll まで (同じ created_at の末尾は次回)・巨大 payload は読まない・fire の例外は on_error") {
    w3n::TempDir dir;
    w3n::TestConfig cfg(dir, false);
    auto core = core::create_core(dir.file("db.kuzu"), dir.file("events.jsonl"));
    StubEngine stub;
    std::vector<std::string> errors;
    EventBridgeOptions bo;
    bo.workspaceId = ws;
    bo.pollMs = 60'000;
    bo.onError = [&](const std::string& e, const DiscatierEventRow*) { errors.push_back(e); };
    auto bridge = create_event_bridge(*core, stub.bridge(), bo);
    const double base = bridge->last_seen_at() + 1000;
    // events 表へ直接: kMax ちょうど + 末尾 2 件が同じ created_at。
    core->raw().transaction([&] {
        for (std::size_t i = 0; i < kMaxEventsPerPoll; ++i)
            core->raw().run("INSERT INTO events (id, event_type, payload_json, created_at) VALUES (?, 'Tick', '{}', ?)",
                            {"e" + std::to_string(100000 + i), base + static_cast<double>(i)});
        core->raw().run("INSERT INTO events (id, event_type, payload_json, created_at) VALUES ('zz1', 'Tick', '{}', ?)",
                        {base + static_cast<double>(kMaxEventsPerPoll - 1)});
    });
    // 末尾 (kMax-1 番目の created_at) の 2 件を次回に回すので kMax-1 件。
    CHECK_EQ(bridge->poll_once(), static_cast<int>(kMaxEventsPerPoll - 1));
    CHECK_EQ(bridge->poll_once(), 2);
    CHECK_EQ(bridge->poll_once(), 0);

    // 巨大 payload (上限 + 1) は object として読まない (session 解決なし・落ちない)。
    std::string big = R"({"sessionId":"s","x":")" + std::string(kEventPayloadLimits.max_bytes, 'a') + "\"}";
    core->raw().run("INSERT INTO events (id, event_type, payload_json, created_at) VALUES ('big', 'Tick', ?, ?)", {big, base + 1e7});
    { std::lock_guard l(stub.m); stub.fired.clear(); }
    CHECK_EQ(bridge->poll_once(), 1);
    { std::lock_guard l(stub.m); CHECK(!stub.fired.at(0).payload); }
    CHECK(!parse_event_payload("{broken").has_value());
    CHECK(!parse_event_payload("[1]").has_value());
    CHECK(parse_event_payload("{}").has_value());

    // fire の例外は on_error に流し、その行の lastSeen は進めない (後続の行は処理する)。
    auto throwing = stub.bridge();
    throwing.fireEvent = [](const pe::engine::EngineEvent& e) {
        if (e.kind == "Boom") throw std::runtime_error("boom");
    };
    auto bridge2 = create_event_bridge(*core, throwing, bo);
    core->raw().run("INSERT INTO events (id, event_type, payload_json, created_at) VALUES ('b1', 'Boom', '{}', ?)", {base + 2e7});
    core->raw().run("INSERT INTO events (id, event_type, payload_json, created_at) VALUES ('b2', 'Ok', '{}', ?)", {base + 2e7 + 1});
    CHECK_EQ(bridge2->poll_once(), 1);
    CHECK(!errors.empty() && errors.back() == "boom");
    CHECK_EQ(bridge2->last_seen_at(), base + 2e7 + 1);
    bridge2->stop();
    bridge->stop();
    core->close();
}

TEST_CASE("event-bridge: start で global_scheduler から poll される・stop で止まる・二重 start しない") {
    w3n::TempDir dir;
    w3n::TestConfig cfg(dir, false);
    auto core = core::create_core(dir.file("db.kuzu"), dir.file("events.jsonl"));
    StubEngine stub;
    auto bridge = create_event_bridge(*core, stub.bridge(), {ws, 20, std::nullopt, {}});
    bridge->start();
    bridge->start();
    tick();
    make_gap(*core, "scheduled");
    for (int i = 0; i < 300 && stub.count("DesignGapDetected") == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK_EQ(stub.count("DesignGapDetected"), std::size_t{1});
    bridge->stop();
    const auto n = stub.count("DesignGapDetected");
    make_gap(*core, "after stop");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK_EQ(stub.count("DesignGapDetected"), n);
    core->close();
}

DI_TEST_MAIN()
