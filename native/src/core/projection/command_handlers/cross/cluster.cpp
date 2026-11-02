#include "core/projection/command_handlers/cross/cluster.hpp"

#include <unordered_map>

#include "base/strings.hpp"

namespace di::core::projection::command_handlers::cross {

namespace {
constexpr sql::QueryLimits kClusterRowLimits{500'000, 256u << 20};

/** raw.split(/\s+/)[0] — 先頭から最初の空白 (JS の \s) までの部分。 */
std::string first_word(std::string_view text) {
    std::size_t i = 0;
    while (i < text.size()) {
        std::size_t next = i;
        const auto cp = str::next_codepoint(text, next);
        if (str::is_js_space(cp)) break;
        i = next;
    }
    return std::string(text.substr(0, i));
}
} // namespace

std::vector<cache::Cluster> run_uncategorized_clustering_job(ReturnTypeCreateCore& core, const std::string& workspace_id) {
    const auto rows = core.raw().all(
        "SELECT id,raw_content FROM utterances WHERE workspace_id = ? AND normalized_content LIKE '%[uncategorized]%' ORDER BY created_at DESC",
        {workspace_id}, kClusterRowLimits);
    // Map の挿入順を保つ。
    std::vector<std::pair<std::string, std::vector<std::string>>> groups;
    std::unordered_map<std::string, std::size_t> index;
    for (const auto& r : rows) {
        std::string word = first_word(r.str("raw_content"));
        const std::string key = str::to_lower(word.empty() ? std::string("misc") : word);
        auto it = index.find(key);
        if (it == index.end()) {
            index.emplace(key, groups.size());
            groups.push_back({key, {}});
            it = index.find(key);
        }
        groups[it->second].second.push_back(r.str("id"));
    }
    std::vector<cache::Cluster> clusters;
    for (auto& [k, ids] : groups) {
        cache::Cluster c;
        c.key = k;
        c.count = static_cast<double>(ids.size());
        for (std::size_t i = 0; i < ids.size() && i < 3; ++i) c.sample.push_back(ids[i]);
        clusters.push_back(std::move(c));
    }
    cache::set_uncategorized_clusters_cache(workspace_id, clusters);
    return clusters;
}

std::string handle_cluster(ReturnTypeCreateCore& core, const std::string& workspace_id) {
    auto cached = cache::get_uncategorized_clusters_cache(workspace_id);
    const auto clusters = cached ? *cached : run_uncategorized_clustering_job(core, workspace_id);
    if (clusters.empty()) return "no clusters";
    std::vector<std::string> lines;
    for (const auto& c : clusters) lines.push_back("- " + c.key + ": " + js_number_to_string(c.count));
    return str::join(lines, "\n");
}

} // namespace di::core::projection::command_handlers::cross
