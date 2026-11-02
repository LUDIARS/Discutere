#include "discussion_seed/seed.hpp"

#include <stdexcept>

#include "base/time.hpp"
#include "persona_engine/facilitator/facilitator.hpp"

namespace di::discussion_seed {

namespace pf = persona_engine::facilitator;

Json to_json(const SeededDiscussion& s) { return Json{{"gapId", s.gapId}, {"sessionId", s.sessionId}}; }

std::string build_opening(const SeedTopic& topic) {
    return "今日のお題は「" + topic.title + "」です。\n" + topic.description + "\n" +
           "それぞれの立場から、 思うところを聞かせてください。 まずは率直な意見からどうぞ。";
}

SeededDiscussion seed_headless_discussion(const SeedHeadlessDiscussionArgs& args) {
    if (!args.core || !args.personas) throw std::invalid_argument("seed_headless_discussion: core and personas required");
    auto& core = *args.core;

    // 1) design gap (headless = discord 紐付けの guild/channel を持たない)
    core::repositories::DesignGapInput gap;
    gap.workspaceId = args.workspaceId;
    gap.gameId = args.gameId;
    gap.title = args.topic.title;
    gap.description = args.topic.description;
    gap.status = "open";
    const std::string gap_id = core.repos.designGap.create(gap);

    // 出自を evidence に残す (discord guild/channel は意図的に入れない = scene が gap: になる)。
    core.raw().run("UPDATE design_gaps SET evidence_json = ?, updated_at = ? WHERE id = ?",
                   {json_stringify(Json{{"source", args.origin.value_or("auto:headless")}}), time::now_ms(), gap_id});

    // 2) 議論 session (scene=gap:<gapId> → discussion-bridge は post を skip = headless)
    core::repositories::SessionInput session;
    session.workspaceId = args.workspaceId;
    session.title = "discussion-of-gap:" + gap_id;
    session.startedAt = static_cast<double>(time::now_ms());
    session.scene = "gap:" + gap_id;
    const std::string session_id = core.repos.session.create(session);

    // 3) 進行役を登録して「登場」 — 開幕の一言で議題を提示する。
    args.personas->insert_or_ignore(pf::FACILITATOR_PERSONA());
    core::repositories::UtteranceInput utt;
    utt.workspaceId = args.workspaceId;
    utt.sessionId = session_id;
    utt.speakerId = std::string("persona:") + pf::FACILITATOR_PERSONA_ID;
    utt.rawContent = build_opening(args.topic);
    utt.postedAt = static_cast<double>(time::now_ms());
    core.repos.utterance.create(utt);

    return {gap_id, session_id};
}

} // namespace di::discussion_seed
