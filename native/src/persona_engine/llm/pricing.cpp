#include "persona_engine/llm/pricing.hpp"

#include "base/strings.hpp"

namespace di::persona_engine {

namespace {
constexpr double MTOK = 1'000'000;

struct TokenRates {
    double input, output, cacheRead, cacheWrite;
};

TokenRates rates(double input_per_m, double output_per_m) {
    return {input_per_m / MTOK, output_per_m / MTOK, (input_per_m * 0.1) / MTOK, (input_per_m * 1.25) / MTOK};
}
} // namespace

std::optional<std::string> model_family(const std::optional<std::string>& model) {
    if (!model || model->empty()) return std::nullopt;
    // model spec (`<model>@<effort>`) の effort 部は無視する。
    const std::string lower = str::to_lower(*model);
    const std::string m = lower.substr(0, lower.find('@'));
    if (str::contains(m, "opus")) return "opus";
    if (str::contains(m, "sonnet")) return "sonnet";
    if (str::contains(m, "haiku")) return "haiku";
    return std::nullopt;
}

std::optional<double> estimate_cost_usd(const std::optional<std::string>& model, const std::optional<TokenUsage>& usage) {
    if (!usage) return std::nullopt;
    const auto family = model_family(model);
    if (!family) return std::nullopt;
    const TokenRates r = *family == "opus" ? rates(5, 25) : *family == "sonnet" ? rates(3, 15) : rates(1, 5);
    const double input = usage->input_tokens.value_or(0);
    const double output = usage->output_tokens.value_or(0);
    const double cache_read = usage->cache_read_input_tokens.value_or(0);
    const double cache_write = usage->cache_creation_input_tokens.value_or(0);
    if (input == 0 && output == 0 && cache_read == 0 && cache_write == 0) return std::nullopt;
    return input * r.input + output * r.output + cache_read * r.cacheRead + cache_write * r.cacheWrite;
}

} // namespace di::persona_engine
