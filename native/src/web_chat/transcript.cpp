#include "web_chat/transcript.hpp"

#include "base/strings.hpp"
#include "web_chat/session.hpp"

namespace di::web_chat {

namespace {

ChatRole classify_role(const std::optional<std::string>& speaker_id) {
    if (!speaker_id || speaker_id->empty()) return "facilitator";  // !speakerId ("" も偽)
    if (str::starts_with(*speaker_id, "persona:")) return "persona";
    if (str::starts_with(*speaker_id, "ext:")) return "external";
    return "human";
}

std::string resolve_speaker(const ChatRole& role, const std::optional<std::string>& speaker_id, const SpeakerNameResolver& resolve) {
    if (role == "persona") {
        const std::string sid = speaker_id.value_or("");
        const std::string pid = sid.size() > 8 ? sid.substr(8) : std::string();
        if (resolve) return resolve(pid);
        return pid.empty() ? "論者" : pid;
    }
    if (role == "external") return "外部の声";
    if (role == "facilitator") return "進行役";
    // speakerId || "あなた" ("" は偽)
    return (speaker_id && !speaker_id->empty()) ? *speaker_id : "あなた";
}

} // namespace

Json to_json(const ChatMessage& m) {
    return Json{{"id", m.id}, {"role", m.role}, {"speaker", m.speaker}, {"content", m.content}, {"postedAt", m.postedAt}};
}

std::vector<ChatMessage> read_room_transcript(sql::Database& raw, const std::string& workspace_id, const std::string& room_id,
                                              const ReadTranscriptOptions& options) {
    const auto rows = raw.all(
        "SELECT u.id AS id, u.speaker_id AS speakerId, u.raw_content AS content,\n"
        "       u.posted_at AS postedAt, s.title AS sessionTitle\n"
        "  FROM utterances u\n"
        "  JOIN sessions s ON s.id = u.session_id\n"
        " WHERE s.workspace_id = ?\n"
        "   AND s.scene = ?\n"
        "   AND u.posted_at >= ?\n"
        " ORDER BY u.posted_at ASC, u.id ASC\n"
        " LIMIT ?",
        {workspace_id, web_scene(room_id), options.sinceTs, options.limit}, kTranscriptReadLimits);

    std::vector<ChatMessage> messages;
    for (const auto& r : rows) {
        const auto speaker_id = r.opt_str("speakerId");
        const auto role = classify_role(speaker_id);
        const bool from_inbox = str::starts_with(r.str("sessionTitle"), "web-session:");
        // 人間発話は入口 session 由来のみ採用 (議論 session のミラー重複を除外)。
        if (role == "human" && !from_inbox) continue;
        // 非人間発話は議論 session 由来のみ。
        if (role != "human" && from_inbox) continue;
        ChatMessage m;
        m.id = r.str("id");
        m.role = role;
        m.speaker = resolve_speaker(role, speaker_id, options.resolveName);
        m.content = r.str("content");
        m.postedAt = r["postedAt"].to_json();
        messages.push_back(std::move(m));
    }
    return messages;
}

} // namespace di::web_chat
