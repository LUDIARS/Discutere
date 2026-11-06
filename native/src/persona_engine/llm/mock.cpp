#include "persona_engine/llm/mock.hpp"

namespace di::persona_engine {

MockLLMClient::MockLLMClient(std::vector<MockResponder> responders, std::string fallback)
    : responders_(std::move(responders)), fallback_(std::move(fallback)) {}

LLMResult MockLLMClient::invoke(const LLMInvokeArgs& args) {
    MockResponder responder;
    {
        std::lock_guard lock(mutex_);
        last_ = args;
        const auto index = static_cast<std::size_t>(call_count_);
        if (index < responders_.size()) responder = responders_[index];
        call_count_ += 1;
    }
    if (!responder) return LLMResult::success(fallback_);
    auto out = responder(args);
    if (auto* text = std::get_if<std::string>(&out)) return LLMResult::success(*text);
    return std::get<LLMResult>(std::move(out));
}

int MockLLMClient::calls() const {
    std::lock_guard lock(mutex_);
    return call_count_;
}

std::optional<LLMInvokeArgs> MockLLMClient::last_invocation() const {
    std::lock_guard lock(mutex_);
    return last_;
}

} // namespace di::persona_engine
