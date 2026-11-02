#include "fundamentum_export/snapshot.hpp"

namespace di::fundamentum_export {

Json build_discussion_snapshot(const visualize::ConclusionDetail& detail) {
    // to_json(ConclusionDetail) は undefined のフィールド (intendedAffect 等) を出さないので JSON 往復と同じ値になる。
    const Json full = visualize::to_json(detail);
    Json raw = Json::object();
    raw["discussionId"] = detail.gapId;
    raw["kind"] = detail.kind;
    raw["title"] = detail.title;
    raw["conclusion"] = full["conclusion"];
    raw["paper"] = full["paper"];
    raw["aufhebungen"] = full["aufhebungen"];
    raw["topOpinions"] = full["topOpinions"];
    raw["transcript"] = full["transcript"];
    raw["utteranceCount"] = detail.utteranceCount;
    raw["aufhebungCount"] = detail.aufhebungCount;
    raw["updatedAt"] = detail.updatedAt;
    return raw;
}

} // namespace di::fundamentum_export
