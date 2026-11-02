#pragma once
// src/flow/user-voices/preparation.ts の移植: 議論ごとの参考情報と、Voluptas 受付待ちの明示的な選択。
// 声の不足は開始を禁止しない。状態は flow_voice_preparation (session_id, payload JSON) に保存する。
#include <functional>
#include <optional>
#include <string>

#include "base/json.hpp"

namespace di::flow::user_voices {

struct VoiceRequestReceipt {
    bool accepted = false;
    std::string requestId;
};

/**
 * Voluptas への依頼窓口。失敗は例外 (VoiceRequestFailure は kind と deliveryUnknown を持つ。それ以外の例外は "protocol")。
 */
struct VoiceRequestPort {
    std::function<VoiceRequestReceipt(const std::string& request_id, const std::string& theme)> submit;
    std::function<VoiceRequestReceipt(const std::string& request_id)> status;
};

struct VoicePreparation {
    double referenceCount = 0;
    bool priorKnowledge = false;
    std::string requestId;
    /** "not_needed" | "submitting" | "submitted" | "accepted" | "unavailable" | "unknown" */
    std::string requestStatus;
    /** "unselected" | "wait" | "continue" */
    std::string choice;
    /** "down" | "auth" | "configuration" | "protocol" */
    std::optional<std::string> failure;
};

/** 保存形式 (TS の作成時のキー順: referenceCount, priorKnowledge, requestId, choice, requestStatus, failure)。 */
Json to_json(const VoicePreparation& s);
VoicePreparation voice_preparation_from_json(const Json& j);

/** payload 1 件の上限。 */
inline constexpr std::size_t kVoicePreparationMaxBytes = 64 * 1024;

std::optional<VoicePreparation> read_voice_preparation(const std::string& session_id);
void save_voice_preparation(const std::string& session_id, const VoicePreparation& state);

struct PrepareVoiceChoiceArgs {
    std::string sessionId;
    std::string theme;
    double referenceCount = 0;
    bool priorKnowledge = false;
    std::optional<VoiceRequestPort> request;
};

/** 既存の状態があればそれを返す (提出結果不明・再起動でも同じ依頼を再送しない)。 */
VoicePreparation prepare_voice_choice(const PrepareVoiceChoiceArgs& args);

bool needs_voice_choice(const std::optional<VoicePreparation>& state);

std::string voice_preparation_notice(const VoicePreparation& state);

std::optional<VoicePreparation> check_voice_receipt(const std::string& session_id, const std::optional<VoiceRequestPort>& request);

} // namespace di::flow::user_voices
