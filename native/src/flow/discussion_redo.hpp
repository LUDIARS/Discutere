#pragma once
// src/flow/discussion-redo.ts の移植: 途中で止まった議論の「やり直し」判定 (純関数)。
// 議論の進行状態はプロセスのメモリにしか無いため、再起動で失われると続きからは再開できない。
// 確定済みのペーパーで議論を最初から回し直す。
#include <optional>
#include <string>
#include <string_view>

namespace di::flow {

/** やり直しの合図とみなす返信 (trim 後の先頭一致: 再開 / やり直し / やりなおし / 再議論 / もう一度 / もういちど)。 */
bool is_redo_text(std::string_view text);

struct RedoPaper {
    std::string status;
    std::string flow;
};

/** { ok: true } | { ok: false, reason: "no-paper" | "not-started" | "concluded" | "running" | "unsupported-flow" } */
struct RedoDecision {
    bool ok = false;
    std::string reason;
};

struct RedoInput {
    std::optional<RedoPaper> paper;
    bool concluded = false;
    bool running = false;
};

/** やり直してよいか: 開始済み (status='started')・結論なし・このプロセスで進行中でないこと。下書きは対象外。 */
RedoDecision decide_redo(const RedoInput& input);

} // namespace di::flow
