#pragma once
// Discatier ↔ persona-engine 接続層 (glue, src/discatier-engine-adapter/index.ts)。
//
// persona-engine は Discatier の具体型を知らない。ここで DiscussionContextProvider を実装し、
// Discatier Core repo に読み書きする橋渡しを行う。
//
// 外部の声 (list_relevant_external_voices) は出所 (source / sourceUrl) を開示し、個人アンカー
// (speaker_id = 公開 ID) は仮名 (`論者#xxxxxx`) にマスクする (EXTERNAL-SOURCES.md §6)。
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "core/index.hpp"
#include "persona_engine/context_provider.hpp"

namespace di::discatier_engine_adapter {

struct PostedUtterance {
    std::string workspaceId;
    std::string sessionId;
    std::string text;
    std::string byPersonaId;
    std::string utteranceId;
};

struct PostedHypothesis {
    std::string workspaceId;
    std::string statement;
    std::string byPersonaId;
    std::optional<std::string> designGapId;
    std::string hypothesisId;
};

struct DiscatierAdapterOptions {
    /** session を 1 つ pin して utterance 投稿先にする (Phase 0 簡易) */
    std::optional<std::string> defaultSessionId;
    /**
     * PR-E #5-2: persona 発言の md 自動 export 先 root (= data/discussions の親)。
     * 指定時、post_utterance / propose_hypothesis 後に該当 md を生成する。
     * 失敗は on_auto_export_error のみで投稿自体は成功扱い (= 議論を止めない)。
     */
    std::optional<std::string> autoExportRootDir;
    /** auto-export / callback 失敗時のログハンドラ (default: log::warn)。type は "utt" | "hyp"。 */
    std::function<void(const std::string& error, const std::string& type, const std::string& id)> onAutoExportError;
    /**
     * PR-I: 議論 utterance / hypothesis を作った直後に呼ぶ callback (呼んだスレッドで呼ぶ)。
     * Discord 議論 bridge から差し込む (= AI 発話を Discord channel に投稿)。重い I/O は callback 側で
     * background_executor に渡すこと。例外は on_auto_export_error に流して無視する (= 議論を止めない)。
     */
    std::function<void(const PostedUtterance&)> onPostedUtterance;
    std::function<void(const PostedHypothesis&)> onPostedHypothesis;
};

/** core は返した provider より長く生きること (呼び出し側が持つ)。 */
std::shared_ptr<persona_engine::DiscussionContextProvider> create_discatier_context_provider(core::Core& core,
                                                                                             DiscatierAdapterOptions options = {});

/** 外部の声 1 件の本文上限 (prompt トークン節約。UTF-16 単位、超えたら切って "…")。 */
inline constexpr std::size_t EXTERNAL_VOICE_CONTENT_CAP = 200;

} // namespace di::discatier_engine_adapter
