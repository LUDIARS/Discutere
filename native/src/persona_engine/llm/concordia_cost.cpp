#include "persona_engine/llm/concordia_cost.hpp"

#include "base/env.hpp"
#include "base/executor.hpp"
#include "base/fsutil.hpp"
#include "base/strings.hpp"
#include "infra/http_client.hpp"

namespace di::persona_engine {
namespace {
std::string base_url() {
    if (auto direct = env::get_nonempty("CONCORDIA_URL")) {
        std::string u = str::trim(*direct);
        while (!u.empty() && u.back() == '/') u.pop_back();
        return u;
    }
    const std::string host = env::get_nonempty("CONCORDIA_HOST").value_or("127.0.0.1");
    const std::string port = env::get_nonempty("CONCORDIA_PORT").value_or("11111");
    return "http://" + str::trim(host) + ":" + str::trim(port);
}
std::string queue_path() {
    if (auto p = env::get_nonempty("CONCORDIA_COST_ONESHOT_QUEUE")) return str::trim(*p);
    return fs::join(fs::join(env::cwd(), "logs"), "cost-one-shot-queue.jsonl");
}
} // namespace

Json concordia_cost_to_json(const ConcordiaCostOneShot& p) {
    Json j = Json::object();
    j["service"] = p.service;
    j["provider"] = p.provider;
    j["command"] = p.command;
    j["model"] = p.model ? Json(*p.model) : Json(nullptr);
    if (p.cwd) j["cwd"] = *p.cwd;
    j["prompt"] = p.prompt;
    j["status"] = p.status;
    if (p.exit_code) j["exit_code"] = *p.exit_code;
    if (p.duration_ms) j["duration_ms"] = *p.duration_ms;
    if (p.input_tokens) j["input_tokens"] = *p.input_tokens;
    if (p.output_tokens) j["output_tokens"] = *p.output_tokens;
    if (p.total_tokens) j["total_tokens"] = *p.total_tokens;
    if (p.cost_usd) j["cost_usd"] = *p.cost_usd;
    if (p.metadata) j["metadata"] = *p.metadata;
    return j;
}

void report_concordia_cost_one_shot(ConcordiaCostOneShot payload) {
    const std::string body = json_stringify(concordia_cost_to_json(payload));
    // 送れない (キュー満杯) ときは同期でファイルに追記する。
    const bool queued = background_executor().submit([body] {
        http::ClientRequest req;
        req.method = "POST";
        req.url = base_url() + "/v1/cost/one-shots";
        req.headers.set("content-type", "application/json");
        req.body = body;
        req.timeout = std::chrono::milliseconds(10'000);
        req.connect_timeout = std::chrono::milliseconds(3'000);
        req.allow_private_network = true;  // Concordia は同一端末 (設定済みのローカルサービス)
        req.max_response_bytes = 64 * 1024;
        req.max_redirects = 0;
        const auto res = http::fetch(req);
        if (!res.ok()) fs::append_file(queue_path(), body + "\n");
    });
    if (!queued) fs::append_file(queue_path(), body + "\n");
}

} // namespace di::persona_engine
