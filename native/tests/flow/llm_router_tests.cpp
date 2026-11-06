// w2-H: src/flow/{llm-fallback,model-router}.ts の移植の確認 (tests/flow/model-roster.test.ts の router 節)。
#include "check.hpp"

#include <memory>
#include <vector>

#include "flow/llm_fallback.hpp"
#include "flow/model_router.hpp"

using namespace di;
using namespace di::persona_engine;

namespace {

struct Tagged : LLMClient {
    std::string tag;
    bool ok;
    std::vector<std::string>* calls;
    Tagged(std::string t, bool o, std::vector<std::string>* c) : tag(std::move(t)), ok(o), calls(c) {}
    LLMResult invoke(const LLMInvokeArgs& a) override {
        calls->push_back(tag + ":" + a.model.value_or(""));
        return ok ? LLMResult::success(tag) : LLMResult::failure(tag + " failed");
    }
};

} // namespace

TEST_CASE("model router: gpt-* goes to codex, others to claude, no codex = error (no impersonation)") {
    std::vector<std::string> calls;
    flow::ModelRouterLlm router(std::make_shared<Tagged>("claude", true, &calls), std::make_shared<Tagged>("codex", true, &calls));
    LLMInvokeArgs a;
    a.prompt = "p";
    a.model = "gpt-5.6-sol@xhigh";
    CHECK(router.invoke(a).ok);
    a.model = "claude-opus-5@xhigh";
    CHECK(router.invoke(a).ok);
    a.model.reset();
    CHECK(router.invoke(a).ok);
    CHECK_EQ(calls, (std::vector<std::string>{"codex:gpt-5.6-sol@xhigh", "claude:claude-opus-5@xhigh", "claude:"}));
    flow::ModelRouterLlm no_codex(std::make_shared<Tagged>("claude", true, &calls), nullptr);
    a.model = "gpt-5.6-sol";
    const auto r = no_codex.invoke(a);
    CHECK(!r.ok);
    CHECK_EQ(r.error, std::string("model-router: codex 経路なし"));
    // codex の失敗を claude で代弁しない
    calls.clear();
    flow::ModelRouterLlm failing(std::make_shared<Tagged>("claude", true, &calls), std::make_shared<Tagged>("codex", false, &calls));
    CHECK(!failing.invoke(a).ok);
    CHECK_EQ(calls.size(), 1u);
}

TEST_CASE("fallback llm: primary ok wins, primary failure goes to fallback") {
    std::vector<std::string> calls;
    LLMInvokeArgs a;
    a.prompt = "p";
    flow::FallbackLlm good(std::make_shared<Tagged>("worker", true, &calls), std::make_shared<Tagged>("cli", true, &calls));
    CHECK_EQ(good.invoke(a).text, std::string("worker"));
    flow::FallbackLlm bad(std::make_shared<Tagged>("worker", false, &calls), std::make_shared<Tagged>("cli", true, &calls));
    CHECK_EQ(bad.invoke(a).text, std::string("cli"));
    flow::FallbackLlm both(std::make_shared<Tagged>("worker", false, &calls), std::make_shared<Tagged>("cli", false, &calls));
    CHECK_EQ(both.invoke(a).error, std::string("cli failed"));
    CHECK_EQ(calls, (std::vector<std::string>{"worker:", "worker:", "cli:", "worker:", "cli:"}));
}

DI_TEST_MAIN()
