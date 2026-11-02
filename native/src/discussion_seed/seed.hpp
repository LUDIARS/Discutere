#pragma once
// Headless 議論のシード (#64, src/discussion-seed/seed.ts)。
//
// Discord 投稿を起点とせず、与えられた議題 (ジャンル種 / トレンド種) から design gap + 議論 session
// (scene=gap:<gapId> = 非 discord = headless) を立て、進行役 (facilitator) が開幕の一言を置いて「登場」する。
// 立てた gap は既存の facilitator 機構 (拡張→止揚判定→収束) がそのまま回す。
#include <optional>
#include <string>

#include "base/json.hpp"
#include "core/index.hpp"
#include "discussion_seed/genres.hpp"
#include "persona_engine/db/personas_repo.hpp"

namespace di::discussion_seed {

struct SeedHeadlessDiscussionArgs {
    core::Core* core = nullptr;
    /** persona-engine.db を開いた PersonasRepo (進行役の登録に使う) */
    persona_engine::db::PersonasRepo* personas = nullptr;
    std::string workspaceId;
    SeedTopic topic;
    /** 任意のゲームに紐づく議論の種にする。 */
    std::optional<std::string> gameId;
    /** 議論の出自タグ (evidence_json に残す。例 "auto:genre" / "auto:store-trend")。既定 "auto:headless"。 */
    std::optional<std::string> origin;
};

struct SeededDiscussion {
    std::string gapId;
    std::string sessionId;
};
Json to_json(const SeededDiscussion& s);

SeededDiscussion seed_headless_discussion(const SeedHeadlessDiscussionArgs& args);

/** 開幕の一言 (進行役)。 */
std::string build_opening(const SeedTopic& topic);

} // namespace di::discussion_seed
