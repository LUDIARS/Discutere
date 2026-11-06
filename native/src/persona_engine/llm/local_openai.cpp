#include "persona_engine/llm/local_openai.hpp"

#include <algorithm>

#include "base/json.hpp"
#include "base/strings.hpp"
#include "persona_engine/llm/llm_vg.hpp"

namespace di::persona_engine {

namespace {
JsonLimits response_limits() {
    JsonLimits l;
    l.max_bytes = LOCAL_OPENAI_MAX_RESPONSE_BYTES;
    l.max_elements = 200'000;
    l.max_depth = 64;
    return l;
}

void log_call(const std::string& model, const LLMInvokeArgs& args, bool ok, std::optional<std::string> error,
              std::optional<double> in = std::nullopt, std::optional<double> out = std::nullopt) {
    LlmLogArgs l;
    l.backend = "local-openai";
    l.model = model;
    l.system = args.system;
    l.prompt = args.prompt;
    l.ok = ok;
    l.error = std::move(error);
    l.input_tokens = in;
    l.output_tokens = out;
    log_llm(l);
}
} // namespace

LocalOpenAiClient::LocalOpenAiClient(LocalOpenAiClientOptions options)
    : default_model_(options.defaultModel.value_or("")),
      api_key_(options.apiKey.value_or("")),
      default_timeout_ms_(options.defaultTimeoutMs.value_or(LOCAL_OPENAI_DEFAULT_TIMEOUT_MS)),
      fetch_(options.fetchImpl ? std::move(options.fetchImpl) : std::function<http::ClientResponse(const http::ClientRequest&)>(http::fetch)) {
    // 末尾スラッシュを正規化 (`/v1/` でも `/v1` でも可)。replace(/\/+$/, "")
    base_url_ = options.baseUrl;
    while (!base_url_.empty() && base_url_.back() == '/') base_url_.pop_back();
}

LLMResult LocalOpenAiClient::invoke(const LLMInvokeArgs& args) {
    if (base_url_.empty()) return LLMResult::failure("local LLM baseUrl not set");
    const std::string model = args.model ? *args.model : default_model_;
    if (model.empty()) return LLMResult::failure("local LLM model not set");
    const int max_tokens = args.maxTokens.value_or(LOCAL_OPENAI_DEFAULT_MAX_TOKENS);
    const long long timeout_ms = args.timeoutMs.value_or(default_timeout_ms_);

    Json messages = Json::array();
    if (args.system && !args.system->empty()) messages.push_back(Json{{"role", "system"}, {"content", *args.system}});
    messages.push_back(Json{{"role", "user"}, {"content", args.prompt}});

    http::ClientRequest req;
    req.method = "POST";
    req.url = base_url_ + "/chat/completions";
    req.headers.set("content-type", "application/json");
    if (!api_key_.empty()) req.headers.set("authorization", "Bearer " + api_key_);
    req.body = json_stringify(Json{{"model", model}, {"max_tokens", max_tokens}, {"messages", std::move(messages)}, {"stream", false}});
    req.timeout = std::chrono::milliseconds(timeout_ms);
    req.connect_timeout = std::chrono::milliseconds(std::min<long long>(timeout_ms, 10'000));
    req.max_response_bytes = LOCAL_OPENAI_MAX_RESPONSE_BYTES;
    // 運用者が設定したローカルのサービス (既定 Ollama localhost) なので private ネットワークを許す。
    req.allow_private_network = true;

    http::ClientResponse res;
    try {
        res = fetch_(req);
    } catch (const std::exception& e) {
        log_call(model, args, false, std::string("invoke failed: ") + e.what());
        return LLMResult::failure(std::string("local LLM invoke failed: ") + e.what());
    }
    if (!res.transport_ok) {
        log_call(model, args, false, "invoke failed: " + res.error);
        return LLMResult::failure("local LLM invoke failed: " + res.error);
    }
    if (!res.ok()) return LLMResult::failure("local LLM http " + std::to_string(res.status) + ": " + str::js_slice(res.body, 0, 200));

    std::string error;
    auto json = parse_json(res.body, response_limits(), &error);
    if (!json) {
        log_call(model, args, false, "invoke failed: " + error);
        return LLMResult::failure("local LLM invoke failed: " + error);
    }
    // json.choices?.[0]?.message?.content ?? ""
    std::string text;
    const Json& choices = json_array(*json, "choices");
    if (!choices.empty()) text = json_string(json_object(choices[0], "message"), "content");
    text = str::trim(text);
    if (text.empty()) {
        log_call(model, args, false, "empty text response");
        return LLMResult::failure("local LLM empty text response");
    }
    std::optional<TokenUsage> usage;
    auto u = json->find("usage");
    if (u != json->end() && u->is_object()) {
        TokenUsage t;
        t.input_tokens = json_opt_number(*u, "prompt_tokens");
        t.output_tokens = json_opt_number(*u, "completion_tokens");
        usage = t;
    }
    log_call(model, args, true, std::nullopt, usage ? usage->input_tokens : std::nullopt,
             usage ? usage->output_tokens : std::nullopt);
    return LLMResult::success(text, usage);
}

} // namespace di::persona_engine
