#include "flow/transcript/discord_message.hpp"

#include <exception>

#include "base/log.hpp"
#include "base/time.hpp"
#include "flow/transcript/digest.hpp"
#include "flow/transcript/html.hpp"
#include "flow/transcript/snapshot.hpp"

namespace di::flow::transcript {

/** スレッド id の長さの上限 (Discord の snowflake は 20 桁以下。余裕を見て切る)。 */
constexpr std::size_t kMaxThreadIdInName = 64;

std::string transcript_file_name(const std::string& thread_id) {
    std::string safe;
    for (const char c : thread_id.substr(0, kMaxThreadIdInName)) {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' || c == '_';
        safe.push_back(ok ? c : '_');
    }
    if (safe.empty()) safe = "thread";
    return "discussion-" + safe + ".html";
}

TranscriptMessage build_transcript_message(const TranscriptMessageInput& input) {
    TranscriptSnapshot snap = load_transcript_snapshot(input.sessionId);
    TranscriptMessage msg;
    msg.body = Json::object();
    msg.body["content"] = render_transcript_digest(DigestInput{input.theme, input.flowLabel, snap});
    msg.body["allowed_mentions"] = discord_hook::allowed_mentions_none();
    TranscriptHtmlInput html;
    html.theme = input.theme;
    html.flowLabel = input.flowLabel;
    html.generatedAtMs = input.generatedAtMs;
    html.snapshot = std::move(snap);
    msg.file = discord_hook::FileUpload{transcript_file_name(input.threadId), render_transcript_html(html), "text/html; charset=utf-8"};
    return msg;
}

bool post_transcript_message(discord_hook::DiscordApi& api, const TranscriptMessageInput& input) {
    try {
        const auto msg = build_transcript_message(input);
        api.create_message(input.threadId, msg.body, {msg.file});
        return true;
    } catch (const std::exception& e) {
        log::warn("  flow-live: 議論まとめの投稿失敗 (thread=" + input.threadId + "): " + e.what());
        return false;
    }
}

bool post_concluded_transcript(discord_hook::DiscordApi* api, const std::string& thread_id, const std::string& session_id,
                               const std::string& theme, const std::string& flow_label) {
    if (!api) return false;
    return post_transcript_message(*api, TranscriptMessageInput{thread_id, session_id, theme, flow_label, time::now_ms()});
}

} // namespace di::flow::transcript
