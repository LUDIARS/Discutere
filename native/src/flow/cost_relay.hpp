#pragma once
// src/flow/cost-relay.ts の移植: LLM コストの横断 relay (Push 型集約)。
// llm_call_log を (session × model × backend) 別に集計し、コスト表示面 (Anatomia / Concordia) の cost-feed へ
// 「そのセッションの累積サマリ」を PUSH する。送信失敗は議論を止めない (ログのみ)。
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "base/sqlite.hpp"
#include "flow/cost_report.hpp"
#include "infra/http_client.hpp"

namespace di::flow {

/** 各コスト表示面の cost-feed エンドポイントのパス (受信側 API で固定)。 */
inline constexpr const char* COST_FEED_PATH_ANATOMIA = "/api/cost-feed";
inline constexpr const char* COST_FEED_PATH_CONCORDIA = "/v1/cost-feed";

struct CostFeedTarget {
    std::string baseUrl;
    std::string path;
    std::optional<std::string> label;
};

struct CostFeedUrls {
    std::optional<std::string> anatomiaUrl;
    std::optional<std::string> concordiaUrl;
};

/** 設定された URL の分だけ target を作る (空文字は無視)。 */
std::vector<CostFeedTarget> build_cost_feed_targets(const CostFeedUrls& urls);

/** fetch の注入境界 (テストで差し替える)。既定は http::fetch。 */
using CostFetch = std::function<http::ClientResponse(const http::ClientRequest&)>;

struct PushResult {
    bool ok = false;
    std::optional<double> recorded;
    std::optional<std::string> error;
};

/** 応答の上限と期限 (cost-feed は設定済みのローカルサービス)。 */
inline constexpr std::size_t kCostFeedMaxResponseBytes = 1u << 20;

/** rows を 1 つの target へ送る。失敗は ok:false に畳む。 */
PushResult push_cost_feed(const CostFeedTarget& target, const std::string& service, const std::vector<CostFeedRow>& rows,
                          double ts, const CostFetch& fetch_impl = {});

struct RelayOnceOptions {
    std::vector<CostFeedTarget> targets;
    std::string service;
    std::optional<double> activeSince;
    double ts = 0;
    CostFetch fetchImpl;
};

struct TargetPushResult {
    CostFeedTarget target;
    PushResult push;
};

struct RelayResult {
    std::size_t pushed = 0;
    std::vector<TargetPushResult> results;
    bool ok = true;
};

/** 一回分の relay: 行を一度だけ集計し、全 target へ同じ payload を push する。 */
RelayResult relay_cost_once(sql::Database& db, const RelayOnceOptions& options);

/** 毎 tick 評価される実効設定。 */
struct CostRelayLive {
    bool enabled = false;
    std::vector<CostFeedTarget> targets;
    std::string service;
};

struct StartCostRelayOptions {
    std::function<CostRelayLive()> resolve;
    std::int64_t intervalMs = 60'000;
    std::function<void(const std::string&)> onError;
    std::function<double()> now;
    CostFetch fetchImpl;
};

/**
 * 定期 relay を開始する。戻り値を呼ぶと停止。各 tick は「前回 tick から 1 interval 重ねた時刻」以降に活動した
 * セッションを全 target へ送る。初回は intervalMs×4 さかのぼる。カーソル前進は全 target 成功時のみ。
 * 無効/送信先なしの間は cursor を near-now に保つ。tick は背景の executor で走らせる (前の tick が走っていれば飛ばす)。
 */
std::function<void()> start_cost_relay(sql::DatabasePtr db, StartCostRelayOptions options);

/** テスト用: start_cost_relay の 1 tick を同期で回す状態機械。 */
class CostRelayTicker {
public:
    CostRelayTicker(sql::DatabasePtr db, StartCostRelayOptions options);
    void tick();
    double next_since() const;

private:
    sql::DatabasePtr db_;
    StartCostRelayOptions options_;
    double next_since_;
    mutable std::mutex mutex_;
};

} // namespace di::flow
