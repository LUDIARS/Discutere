#pragma once
// 議論 1 件の表示用スナップショット (発話 + 投票/採択/止揚の印 + 結論 + ペーパー本文) を DB から組み立てる。
//
// GET /api/flow/:session/status と、議論終了時に Discord へ送るチャット風 HTML の両方がこれを使う
// (画面とファイルが同じ中身を見せるため)。プロセス内の進行状態 (壁打ち・未来予測) は含めない。
#include <optional>
#include <string>

#include "base/json.hpp"

namespace di::flow::transcript {

struct TranscriptSnapshot {
    /** status API の utterances と同じ形の配列 (created_at > since を古い順)。 */
    Json utterances = Json::array();
    /** status API の marks と同じ形の配列 (印が付いた発話だけ、全期間)。 */
    Json marks = Json::array();
    /** flow_conclusion.summary (行が無い / NULL なら null)。 */
    Json conclusion = nullptr;
    /** flow_conclusion.concluded == 1。 */
    bool concluded = false;
    /** flow_conclusion の行があるか (status の done 判定に使う)。 */
    bool hasConclusionRow = false;
    /** ライブのディスカッションペーパー本文。 */
    std::optional<std::string> paperMd;
};

/** session_id の議論を読む。since は created_at の下限 (ミリ秒、0 で全件)。 */
TranscriptSnapshot load_transcript_snapshot(const std::string& session_id, double since = 0);

} // namespace di::flow::transcript
