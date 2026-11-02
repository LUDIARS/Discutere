#pragma once
// 未来予測アドバイザリーの進行状態 (セッションごと、プロセス内メモリ。src/flow/foresight/state.ts の移植)。
// 本議論の結論は未来予測より前に書かれるので、結論の有無だけで「完了」と判定すると未来予測の実行中も完了に見える。
// Web の status はこの状態を見て、本議論の完了と未来予測の完了を分けて返す。
// 永続しない: 実行中の未来予測はプロセスと一緒に消えるので、再起動後は状態なし (= 走っていない) として扱う。
#include <optional>
#include <string>

#include "base/json.hpp"

namespace di::flow::foresight {

/** "pending" | "running" | "done" | "skipped" | "degraded" | "failed" */
using ForesightState = std::string;

struct ForesightStatus {
    ForesightState state;
    /** 省略・失敗の理由 (人間向け)。 */
    std::optional<std::string> detail;
    bool operator==(const ForesightStatus&) const = default;
};
Json to_json(const ForesightStatus& s);

/** detail が空なら detail を持たない (TS の `detail ? {state, detail} : {state}`)。 */
void set_foresight_status(const std::string& session_id, const ForesightState& state, const std::string& detail = "");
std::optional<ForesightStatus> get_foresight_status(const std::string& session_id);
/** まだ終わっていない (待機中 / 実行中)。 */
bool is_foresight_active(const std::string& session_id);
/** テスト用。 */
void reset_foresight_status_for_test();

} // namespace di::flow::foresight
