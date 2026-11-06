#include "fundamentum_export/list.hpp"

#include <algorithm>

#include "fundamentum_export/export.hpp"
#include "fundamentum_export/store.hpp"
#include "visualize/locale_compare.hpp"

namespace di::fundamentum_export {

Json to_json(const ExportedDiscussionRef& r) {
    Json o = Json::object();
    o["discussionId"] = r.discussionId;
    o["contentId"] = r.contentId;
    return o;
}

Json to_json(const DiscussionRevision& r) {
    Json o = Json::object();
    o["version"] = r.version;
    o["contentId"] = r.contentId;
    return o;
}

std::vector<ExportedDiscussionRef> list_exported_discussions() {
    auto& fm = get_fundamentum_store();
    const auto catalog = fm.catalog.latest(DISCUSSIONS_CATALOG);
    std::vector<ExportedDiscussionRef> out;
    if (!catalog || !catalog->entries.is_object()) return out;
    for (const auto& [id, content] : catalog->entries.items())
        if (content.is_string()) out.push_back({id, content.get<std::string>()});
    std::stable_sort(out.begin(), out.end(),
                     [](const auto& a, const auto& b) { return visualize::locale_compare(a.discussionId, b.discussionId) < 0; });
    return out;
}

std::optional<Json> get_exported_discussion_snapshot(const std::string& discussion_id) {
    const auto refs = list_exported_discussions();
    const auto it = std::find_if(refs.begin(), refs.end(), [&](const auto& r) { return r.discussionId == discussion_id; });
    if (it == refs.end()) return std::nullopt;
    return get_fundamentum_store().master.get(it->contentId);
}

std::vector<DiscussionRevision> get_discussion_export_history(const std::string& discussion_id) {
    auto& fm = get_fundamentum_store();
    const auto versions = fm.catalog.history(DISCUSSIONS_CATALOG);
    std::vector<DiscussionRevision> out;
    std::optional<std::string> last;
    for (auto it = versions.rbegin(); it != versions.rend(); ++it) {
        if (!it->entries.is_object() || !it->entries.contains(discussion_id)) continue;
        const Json& v = it->entries[discussion_id];
        if (!v.is_string()) continue;
        const std::string content = v.get<std::string>();
        if (!content.empty() && content != last) {
            out.push_back({it->version, content});
            last = content;
        }
    }
    return out;
}

} // namespace di::fundamentum_export
