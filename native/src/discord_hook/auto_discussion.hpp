#pragma once
// Discord 投稿の分類 → designGap (議論の種) 自動起動 (src/discord-hook/auto-discussion.ts)。
//
// 分類は LLM (あれば) か線形の語検索 (fallback)。LLM の応答 JSON は上限つきで読む。
// started=true は「議論の種 (designGap) が新規に立った = 開始エントリ」を意味する。
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/json.hpp"
#include "core/index.hpp"
#include "persona_engine/llm/client.hpp"

namespace di::discord_hook {

/** "game_design_question" | "mechanic_question" | "opinion" | "noise" | "command_like" */
using AutoDiscussionCategory = std::string;

/**
 * 議論の方向性 (フォーラムのタグで指定)。
 * - "improvement": 改善提案 — 課題を洗い出し具体的な改善案を出し合う。
 * - "fun": 面白さ — 何がどう面白いか、体験の魅力を語り合う (タグ無しの既定)。
 */
using ForumDirection = std::string;

/** タグが無い / 判別不能なときの既定方向 (= 面白さ)。 */
inline constexpr const char* DEFAULT_FORUM_DIRECTION = "fun";

/** 方向性を facilitator が読む議題説明に差し込むためのディレクティブ文。 */
std::string forum_direction_directive(const ForumDirection& direction);

struct DiscordAutoDiscussionInput {
    std::string workspaceId;
    std::string guildId;
    std::string channelId;
    std::string sessionId;
    std::string utteranceId;
    std::string authorId;
    std::string content;
    /** フォーラムスレッドのタイトル (= 議論の主題)。フォーラム starter のみ設定。 */
    std::optional<std::string> forumTitle;
    /** 議論の方向性 (フォーラムのタグ由来)。設定時のみ議題説明にディレクティブを差し込む。 */
    std::optional<ForumDirection> direction;
    /** フォーラムの適用タグ名。結論まとめの絞り込み用に gap evidence (`tags`) へ保存する。 */
    std::optional<std::vector<std::string>> forumTags;
};

struct AutoDiscussionClassification {
    /** "start_discussion" | "record_only" */
    std::string action;
    AutoDiscussionCategory category;
    std::string title;
    std::string description;
    /** 議題のゲームタイトル (特定できなければ nullopt)。議論の主題アンカー。 */
    std::optional<std::string> gameTitle;
    std::optional<std::string> genre;
    std::optional<std::string> expectedAffect;
    std::optional<std::string> observedAffect;
    std::optional<std::string> reason;
};
/** 値の無い項目は出さない (TS の undefined)。 */
Json to_json(const AutoDiscussionClassification& c);

/** LLM 応答テキストの上限 (超えたら読まずに fallback)。 */
inline constexpr std::size_t kMaxClassificationTextBytes = 256u << 10;
/** LLM 応答 JSON の上限。 */
inline constexpr JsonLimits kClassificationJsonLimits{kMaxClassificationTextBytes, 10'000, 32};
/** design_gaps.evidence_json の上限 (DB の値も外部入力)。 */
inline constexpr JsonLimits kEvidenceJsonLimits{1u << 20, 100'000, 32};

struct StartAutoDiscussionResult {
    bool started = false;
    std::optional<std::string> gapId;
    AutoDiscussionClassification classification;
};

/**
 * 投稿を分類し、議論に値すれば designGap を立てる。同じ発話から立った gap が既にあれば立てない。
 * LLM の応答に文字列でない title 等があると TS と同じく例外 (std::runtime_error)。
 */
StartAutoDiscussionResult start_auto_discussion_for_discord_message(core::Core& core, const DiscordAutoDiscussionInput& input,
                                                                    const persona_engine::LLMClientPtr& llm = nullptr);

/** LLM (あれば) で分類する。LLM の失敗・読めない応答は fallback の結果を正規化して返す。 */
AutoDiscussionClassification classify_discord_message(const std::string& content, const persona_engine::LLMClientPtr& llm = nullptr,
                                                      const std::optional<std::string>& thread_title = std::nullopt);

/** 語の有無による素朴な分類 (正規表現を使わず部分一致で判定する)。 */
AutoDiscussionClassification classify_discord_message_fallback(const std::string& content,
                                                               const std::optional<std::string>& thread_title = std::nullopt);

/** 応答テキストの最初の `{` から最後の `}` までを JSON として読む (/\{[\s\S]*\}/ と同じ範囲)。読めなければ nullopt。 */
std::optional<Json> parse_classification_json(std::string_view text);

struct DiscordAutoDiscussionStarterOptions {
    std::function<persona_engine::LLMClientPtr()> getLlm;
};

struct StarterResult {
    bool started = false;
};

/** active KG を開いて分類 → 起動し、閉じる関数を作る (gateway が呼ぶ)。 */
std::function<StarterResult(const DiscordAutoDiscussionInput&)> create_discord_auto_discussion_starter(
    DiscordAutoDiscussionStarterOptions options = {});

} // namespace di::discord_hook
