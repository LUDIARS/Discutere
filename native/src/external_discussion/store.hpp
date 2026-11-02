#pragma once
// 参加の試行の永続化 (リース付き)。移植元 src/external-discussion/store.ts。
#include <cstdint>
#include <optional>
#include <string>

#include "base/sqlite.hpp"
#include "external_discussion/contracts.hpp"

namespace di::external_discussion {

/** claim の結果: 既に決まった結果 (result) か、今回の試行の所有トークン (token)。 */
struct ClaimResult {
    std::optional<ParticipationResult> result;
    std::optional<std::string> token;
};

class ParticipationStore {
public:
    explicit ParticipationStore(sql::DatabasePtr db);
    /** BEGIN IMMEDIATE の中で、重複・同時実行・直近の提案を見て試行を取る。 */
    ClaimResult claim(const std::string& scene, const std::string& id, const std::string& human_id, std::int64_t now);
    /** 所有トークンが一致する running を done にする。リースを失っていれば std::runtime_error("discussion lease lost")。 */
    void finish(const std::string& id, const std::string& token, const ParticipationResult& result);
    void fail(const std::string& id, const std::string& token);

private:
    sql::DatabasePtr db_;
};

} // namespace di::external_discussion
