#pragma once
// 共有 KG の pull オーケストレーション (follower 側, src/kg-sync/pull.ts) — spec/data/core/KG-SYNC.md。
//
// 設定 URL に `?since=<watermark>` を付けて GET → NDJSON を import → 透かしを保存。
// URL は master の生エンドポイント / 静的 publish ファイルのどちらでも良い (どちらも GET → NDJSON。
// 静的ホストは since を無視して全件返すが、import 側 LWW で冪等)。
//
// 安全 (TS と同じか強く):
//   - URL は http/https のみ (認証情報入り・その他の scheme は拒否)。運用者が設定した宛先なので private network は許す。
//   - token は ?token= と x-kg-token に載せる。リダイレクトは追わない (token を別の宛先に渡さない。移植で追加)。
//   - 期限 (既定 30 秒)、応答本文は import の上限 (256MiB) まで。エラー文言・ログに URL・token を出さない。
#include <optional>
#include <string>

#include "base/json.hpp"
#include "base/sqlite.hpp"
#include "kg_sync/import.hpp"
#include "kg_sync/state.hpp"

namespace di::kg_sync {

struct KgPullOptions {
    std::string url;
    /** 同期状態ファイル (既定 ./data/kg-sync-state.json)。 */
    std::optional<std::string> statePath;
    /** 配信元が要求する共有シークレット (任意)。 */
    std::optional<std::string> token;
    /** 透かしを無視して全件取り直す (既定 false)。 */
    bool full = false;
    /** fetch タイムアウト ms (既定 30s)。 */
    std::optional<double> timeoutMs;
    /** 取り込みの上限 (応答本文の上限は limits.max_bytes)。 */
    KgImportLimits limits;
};

struct KgPullResult {
    std::string url;
    double fromWatermark = 0;
    double toWatermark = 0;
    double applied = 0;
    double skipped = 0;
    double ignored = 0;
};
Json to_json(const KgPullResult& r);

/** 取得 URL (since / token を付けたもの) を組み立てる。不正な URL は std::invalid_argument("Invalid URL")。 */
std::string build_pull_url(const std::string& url, double since, const std::optional<std::string>& token);

/** 失敗 (HTTP の失敗・通信失敗・上限超過) は std::runtime_error ("KG pull failed: ..."、URL・token を含めない)。 */
KgPullResult pull_shared_kg(sql::Database& db, const KgPullOptions& opts);

} // namespace di::kg_sync
