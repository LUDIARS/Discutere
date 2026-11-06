#pragma once
// src/flow/discord-flow-runs.ts の移植: Discord スレッドと現在の議論を対応付ける。
// 再議論でも以前のペーパー・発言・結論を保持する。
#include <optional>
#include <string>

namespace di::flow {

/** スレッドの現在の session id (対応が無ければ threadId)。 */
std::string forum_session_id(const std::string& thread_id);

/** 修正用の新しい草案を作る。再度の呼び出しは同じ草案を返す。対象外 (議論/改善でない) は nullopt。 */
std::optional<std::string> open_rediscussion(const std::string& thread_id);

} // namespace di::flow
