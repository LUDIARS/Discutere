#include "kg_sync/pull.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>

#include "base/time.hpp"
#include "base/url.hpp"
#include "infra/http_client.hpp"

namespace di::kg_sync {

namespace {

const char* kind_text(http::FetchErrorKind k) {
    switch (k) {
    case http::FetchErrorKind::timeout: return "timeout";
    case http::FetchErrorKind::blocked: return "blocked";
    case http::FetchErrorKind::too_large: return "response too large";
    case http::FetchErrorKind::busy: return "busy";
    case http::FetchErrorKind::dns: return "dns";
    case http::FetchErrorKind::invalid_url: return "invalid url";
    case http::FetchErrorKind::too_many_redirects: return "redirect";
    default: return "network";
    }
}

} // namespace

Json to_json(const KgPullResult& r) {
    return Json{{"url", r.url},       {"fromWatermark", r.fromWatermark}, {"toWatermark", r.toWatermark},
                {"applied", r.applied}, {"skipped", r.skipped},             {"ignored", r.ignored}};
}

std::string build_pull_url(const std::string& url, double since, const std::optional<std::string>& token) {
    const auto u = url::parse(url);
    if (!u || (u->scheme != "http" && u->scheme != "https")) throw std::invalid_argument("Invalid URL");
    auto params = url::parse_query(u->query);
    // URLSearchParams#set: 最初の同名を置き換えて残りを消す。無ければ末尾に足す。
    auto set = [&](const std::string& name, const std::string& value) {
        bool done = false;
        std::vector<std::pair<std::string, std::string>> next;
        for (auto& [k, v] : params) {
            if (k != name) next.emplace_back(k, v);
            else if (!done) {
                next.emplace_back(k, value);
                done = true;
            }
        }
        if (!done) next.emplace_back(name, value);
        params = std::move(next);
    };
    set("since", js_number_to_string(since));
    if (token && !token->empty()) set("token", *token);
    return u->origin() + (u->path.empty() ? "/" : u->path) + "?" + url::build_query(params);
}

KgPullResult pull_shared_kg(sql::Database& db, const KgPullOptions& opts) {
    const std::string state_path = opts.statePath ? *opts.statePath : DEFAULT_SYNC_STATE_PATH;
    auto state = read_sync_state(state_path);
    const double from = opts.full ? 0 : get_source_watermark(state, opts.url);

    http::ClientRequest req;
    req.url = build_pull_url(opts.url, from, opts.token);
    if (opts.token && !opts.token->empty()) req.headers.set("x-kg-token", *opts.token);
    const double timeout = opts.timeoutMs.value_or(30'000);
    req.timeout = std::chrono::milliseconds(static_cast<long long>(std::isfinite(timeout) && timeout > 0 ? timeout : 30'000));
    req.connect_timeout = std::min(req.connect_timeout, req.timeout);
    req.max_response_bytes = opts.limits.max_bytes;
    // 運用者が設定した master (社内・ローカル) へ繋ぐ。token を別の宛先へ渡さないよう追わない。
    req.allow_private_network = true;
    req.max_redirects = 0;
    const auto res = http::fetch(req);
    if (!res.transport_ok) throw std::runtime_error(std::string("KG pull failed: ") + kind_text(res.error_kind));
    if (!res.ok()) throw std::runtime_error("KG pull failed: HTTP " + std::to_string(res.status) + " " + res.reason);

    const auto imported = import_shared_kg(db, res.body, opts.limits);
    const double to = std::max(from, imported.watermark);
    set_source_state(state, opts.url,
                     SourceSyncState{to, static_cast<double>(time::now_ms()), static_cast<double>(imported.applied)});
    write_sync_state(state, state_path);

    KgPullResult r;
    r.url = opts.url;
    r.fromWatermark = from;
    r.toWatermark = to;
    r.applied = static_cast<double>(imported.applied);
    r.skipped = static_cast<double>(imported.skipped);
    r.ignored = static_cast<double>(imported.ignored);
    return r;
}

} // namespace di::kg_sync
