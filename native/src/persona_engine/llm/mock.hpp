#pragma once
// MockLLMClient (src/persona-engine/llm/mock.ts) — テスト / 開発時に LLM を呼ばずスクリプト応答を返す。
// scripted を使い切ったら fallback (既定 {"action":"skip","reasoning":"mock exhausted"})。
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "persona_engine/llm/client.hpp"

namespace di::persona_engine {

using MockResponder = std::function<std::variant<std::string, LLMResult>(const LLMInvokeArgs&)>;

class MockLLMClient : public LLMClient {
public:
    explicit MockLLMClient(std::vector<MockResponder> responders,
                           std::string fallback = R"({"action":"skip","reasoning":"mock exhausted"})");
    LLMResult invoke(const LLMInvokeArgs& args) override;
    /** これまでの呼び出し回数 */
    int calls() const;
    /** 直近の呼び出し引数 */
    std::optional<LLMInvokeArgs> last_invocation() const;

private:
    std::vector<MockResponder> responders_;
    std::string fallback_;
    mutable std::mutex mutex_;
    int call_count_ = 0;
    std::optional<LLMInvokeArgs> last_;
};

} // namespace di::persona_engine
