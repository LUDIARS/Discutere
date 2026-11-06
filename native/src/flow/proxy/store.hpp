#pragma once
// src/flow/proxy/store.ts の移植: 代理議論の人物パックの永続 (flow_proxy_profile)。
//
// 1 行 = 1 話者 × 1 ジャンル。CRUD と「キャストに出してよいか」の読み出し条件だけを持つ。露出名 label は
// speakerKey の一方向 hash で、生の外部 ID はこの表に存在しない (契約が仮名しか通さない)。
// ファイルは読まない。pack_json (DB の値) は上限つきで読み、契約 (parse_persona_pack) で検査し直す。
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "base/json.hpp"
#include "flow/proxy/contract.hpp"

namespace di::flow::proxy {

/** "unevaluated" | "passed" | "failed" */
using ProxyEvalStatus = std::string;

struct ProxyProfileRow {
    std::string speakerKey;
    std::string genre;
    std::string label;
    std::string profileVersion;
    std::string contentHash;
    PersonaPack pack;
    std::string confidence;
    double expiresAt = 0;
    bool revoked = false;
    ProxyEvalStatus evalStatus;
    std::optional<std::string> evalJson;
    double importedAt = 0;
    double updatedAt = 0;
};

Json to_json(const ProxyProfileRow& r);

/** pack_json 1 件の上限。 */
inline constexpr std::size_t kProxyPackJsonMaxBytes = 1u << 20;

/** pack_json が壊れている / 契約に通らない行は std::runtime_error (TS の JSON.parse の例外と同じく投げる)。 */
std::optional<ProxyProfileRow> get_proxy_profile(const std::string& speaker_key, const std::string& genre);

struct UpsertProxyProfileInput {
    PersonaPack pack;
    std::string label;
    std::string contentHash;
    double now = 0;
};

/** 内容 hash が変わったときだけ評価状態を unevaluated に戻す。"inserted" | "updated" | "unchanged"。 */
std::string upsert_proxy_profile(const UpsertProxyProfileInput& input);

/** 失効 (形成側の revoked / オプトアウト)。行は残して以後のキャストから外す。 */
bool revoke_proxy_profile(const std::string& speaker_key, const std::string& genre, double now);

void set_proxy_eval_status(const std::string& speaker_key, const std::string& genre, const ProxyEvalStatus& status,
                           const std::string& eval_json, double now);

/** キャストに出してよい行か。出せない理由 (日本語) か、出せるなら nullopt。 */
std::optional<std::string> is_castable(const ProxyProfileRow& row, double now, bool require_passed);

/** 露出名 (論者#xxxxxx) で引く。genre 指定が無ければ同じ人物の全ジャンル (updated_at の新しい順)。 */
std::vector<ProxyProfileRow> find_proxy_profiles_by_label(const std::string& label, const std::optional<std::string>& genre = std::nullopt);

struct ListProxyProfilesOpts {
    std::optional<std::string> genre;
    bool includeRevoked = false;
};

std::vector<ProxyProfileRow> list_proxy_profiles(const ListProxyProfilesOpts& opts = {});

} // namespace di::flow::proxy
