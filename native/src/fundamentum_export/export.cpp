#include "fundamentum_export/export.hpp"

#include "fundamentum_export/snapshot.hpp"
#include "fundamentum_export/store.hpp"

namespace di::fundamentum_export {

Json to_json(const FundamentumExportResult& r) {
    Json o = Json::object();
    o["discussionId"] = r.discussionId;
    o["contentId"] = r.contentId;
    o["catalogVersion"] = r.catalogVersion;
    return o;
}

FundamentumExportResult export_discussion_to_fundamentum(const visualize::ConclusionDetail& detail) {
    auto& fm = get_fundamentum_store();
    const Json snapshot = build_discussion_snapshot(detail);
    const auto record = fm.master.put(snapshot);
    const auto catalog = fm.catalog.builder(DISCUSSIONS_CATALOG).set(detail.gapId, record.id).commit();
    return {detail.gapId, record.id, catalog.version};
}

} // namespace di::fundamentum_export
