#pragma once
// 議論終了時の Discord 投稿 1 通 (まとめ本文 + チャット風 HTML の添付) を組み立てて送る。
#include <cstdint>
#include <optional>
#include <string>

#include "base/json.hpp"
#include "discord_hook/discord_api.hpp"

namespace di::flow::transcript {

struct TranscriptMessageInput {
    /** 投稿先スレッド (添付のファイル名にも使う)。 */
    std::string threadId;
    /** flow_* テーブルの session_id。 */
    std::string sessionId;
    std::string theme;
    std::string flowLabel;
    std::optional<std::int64_t> generatedAtMs;
};

struct TranscriptMessage {
    /** create_message の本文 (content + allowed_mentions)。 */
    Json body;
    discord_hook::FileUpload file;
};

/** 添付のファイル名 discussion-<thread>.html (英数字・- _ 以外は _ にする)。 */
std::string transcript_file_name(const std::string& thread_id);

/** DB から議論を読み、まとめ本文と HTML 添付を作る。 */
TranscriptMessage build_transcript_message(const TranscriptMessageInput& input);

/** 組み立てて投稿する。失敗は warn ログにして例外を出さない (議論の締めを止めない)。投稿できたら true。 */
bool post_transcript_message(discord_hook::DiscordApi& api, const TranscriptMessageInput& input);

/** 議論の締めで呼ぶ入口。出力時刻は今。api が無ければ何もしない (false)。
 *  呼び出し側で入力の構造体を組み立てない (大きい関数の静的解析を膨らませない) ため、引数で受ける。 */
bool post_concluded_transcript(discord_hook::DiscordApi* api, const std::string& thread_id, const std::string& session_id,
                               const std::string& theme, const std::string& flow_label);

} // namespace di::flow::transcript
