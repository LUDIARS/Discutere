#include "flow/cost_relay.hpp"

#include <algorithm>
#include <atomic>
#include <memory>

#include "base/executor.hpp"
#include "base/log.hpp"
#include "base/time.hpp"

namespace di::flow {

std::vector<CostFeedTarget> build_cost_feed_targets(const CostFeedUrls& urls) {
    std::vector<CostFeedTarget> out;
    if (urls.anatomiaUrl && !urls.anatomiaUrl->empty())
        out.push_back({*urls.anatomiaUrl, COST_FEED_PATH_ANATOMIA, std::string("anatomia")});
    if (urls.concordiaUrl && !urls.concordiaUrl->empty())
        out.push_back({*urls.concordiaUrl, COST_FEED_PATH_CONCORDIA, std::string("concordia")});
    return out;
}

PushResult push_cost_feed(const CostFeedTarget& target, const std::string& service, const std::vector<CostFeedRow>& rows,
                          double ts, const CostFetch& fetch_impl) {
    // baseUrl.replace(/\/+$/, "") + path
    std::string base = target.baseUrl;
    while (!base.empty() && base.back() == '/') base.pop_back();
    Json sessions = Json::array();
    for (const auto& r : rows) sessions.push_back(to_json(r));
    Json body = Json::object();
    body["service"] = service;
    body["ts"] = ts;
    body["sessions"] = std::move(sessions);

    http::ClientRequest req;
    req.method = "POST";
    req.url = base + target.path;
    req.headers.set("content-type", "application/json");
    req.body = json_stringify(body);
    req.timeout = std::chrono::milliseconds(15'000);
    req.max_response_bytes = kCostFeedMaxResponseBytes;
    req.allow_private_network = true;  // 設定済みのコスト表示面 (Anatomia / Concordia) はローカルで動く
    PushResult out;
    try {
        const auto res = fetch_impl ? fetch_impl(req) : http::fetch(req);
        if (!res.transport_ok) {
            out.error = res.error.empty() ? std::string("fetch failed") : res.error;
            return out;
        }
        if (!res.ok()) {
            out.error = "http " + std::to_string(res.status);
            return out;
        }
        out.ok = true;
        // res.json().catch(() => ({})) — 読めなければ recorded なし
        if (auto j = parse_json(res.body, {kCostFeedMaxResponseBytes, 10'000, 16}); j && j->is_object()) {
            auto it = j->find("recorded");
            if (it != j->end() && it->is_number()) out.recorded = it->get<double>();
        }
        return out;
    } catch (const std::exception& e) {
        out.ok = false;
        out.error = e.what();
        return out;
    }
}

RelayResult relay_cost_once(sql::Database& db, const RelayOnceOptions& options) {
    CostReportFilter filter;
    filter.activeSince = options.activeSince;
    const auto rows = cost_feed_rows(db, filter);
    RelayResult r;
    if (rows.empty() || options.targets.empty()) return r;
    for (const auto& target : options.targets)
        r.results.push_back({target, push_cost_feed(target, options.service, rows, options.ts, options.fetchImpl)});
    r.pushed = rows.size();
    for (const auto& x : r.results) r.ok = r.ok && x.push.ok;
    return r;
}

CostRelayTicker::CostRelayTicker(sql::DatabasePtr db, StartCostRelayOptions options)
    : db_(std::move(db)), options_(std::move(options)) {
    if (!options_.now) options_.now = [] { return static_cast<double>(time::now_ms()); };
    if (!options_.onError) options_.onError = [](const std::string& m) { log::error("[cost-relay] " + m); };
    next_since_ = options_.now() - static_cast<double>(options_.intervalMs) * 4;
}

double CostRelayTicker::next_since() const {
    std::lock_guard lock(mutex_);
    return next_since_;
}

void CostRelayTicker::tick() {
    const double start = options_.now();
    const CostRelayLive live = options_.resolve ? options_.resolve() : CostRelayLive{};
    const double interval = static_cast<double>(options_.intervalMs);
    if (!live.enabled || live.targets.empty()) {
        std::lock_guard lock(mutex_);
        next_since_ = start - interval;  // 無効中は cursor を near-now に保つ
        return;
    }
    double since;
    {
        std::lock_guard lock(mutex_);
        since = next_since_;
    }
    RelayOnceOptions o;
    o.targets = live.targets;
    o.service = live.service;
    o.activeSince = since;
    o.ts = start;
    o.fetchImpl = options_.fetchImpl;
    RelayResult r;
    try {
        r = relay_cost_once(*db_, o);
    } catch (const std::exception& e) {
        options_.onError(std::string("relay failed: ") + e.what());
        return;
    }
    for (const auto& x : r.results)
        if (!x.push.ok) options_.onError("push to " + x.target.label.value_or(x.target.baseUrl) + " failed: " + x.push.error.value_or(""));
    if (r.ok) {
        std::lock_guard lock(mutex_);
        next_since_ = start - interval;  // 次回は 1 interval 重ねる
    }
}

std::function<void()> start_cost_relay(sql::DatabasePtr db, StartCostRelayOptions options) {
    const auto interval = std::chrono::milliseconds(std::max<std::int64_t>(1, options.intervalMs));
    auto ticker = std::make_shared<CostRelayTicker>(std::move(db), std::move(options));
    auto busy = std::make_shared<std::atomic<bool>>(false);
    auto stopped = std::make_shared<std::atomic<bool>>(false);
    // タイマーは短く: 実際の集計と送信は背景の executor で。前の tick が走っていれば飛ばす。
    auto fire = [ticker, busy, stopped] {
        if (stopped->load() || busy->exchange(true)) return;
        const bool queued = background_executor().submit([ticker, busy, stopped] {
            if (!stopped->load()) ticker->tick();
            busy->store(false);
        });
        if (!queued) busy->store(false);
    };
    const auto id = global_scheduler().every(interval, fire, /*run_now=*/true);
    return [id, stopped] {
        stopped->store(true);
        global_scheduler().cancel(id);
    };
}

} // namespace di::flow
