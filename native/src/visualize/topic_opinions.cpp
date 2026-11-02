#include "visualize/topic_opinions.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

#include "base/time.hpp"

namespace di::visualize {

std::int64_t clamp_limit(double value, std::int64_t min, std::int64_t max) {
    if (!std::isfinite(value)) return min;
    const double f = std::floor(value);
    return static_cast<std::int64_t>(std::min(static_cast<double>(max), std::max(static_cast<double>(min), f)));
}

Json build_topic_opinion_snapshot(sql::Database& db, const std::string& workspace_id, const TopicOpinionOptions& options) {
    const std::int64_t limit = clamp_limit(options.limit.value_or(40), 1, 200);
    const std::int64_t per_topic = clamp_limit(options.opinionsPerTopic.value_or(8), 1, 50);

    const auto topic_rows = db.all(
        "SELECT id, title, description, status, gap_in, expected_affect, observed_affect, updated_at\n"
        "         FROM design_gaps\n"
        "        WHERE workspace_id = ?\n"
        "        ORDER BY updated_at DESC\n"
        "        LIMIT ?",
        {workspace_id, limit});

    struct Topic {
        Json json;
        double size;
        double updated_at;
    };
    std::vector<Topic> topics;
    std::int64_t total_opinions = 0, total_utterances = 0;
    for (const auto& row : topic_rows) {
        const std::string id = row.str("id");
        Json opinions = Json::array();
        for (const auto& o : db.all(
                 "SELECT id, statement, status, integrated, updated_at\n"
                 "       FROM hypotheses\n"
                 "      WHERE workspace_id = ? AND design_gap_id = ?\n"
                 "      ORDER BY updated_at DESC\n"
                 "      LIMIT ?",
                 {workspace_id, id, per_topic})) {
            Json j = Json::object();
            j["id"] = o["id"].to_json();
            j["statement"] = o["statement"].to_json();
            j["status"] = o["status"].to_json();
            j["integrated"] = o["integrated"].is_number() && o["integrated"].as_double() == 1.0;
            j["updatedAt"] = o["updated_at"].to_json();
            opinions.push_back(std::move(j));
        }
        const auto oc = db.get("SELECT COUNT(*) AS c FROM hypotheses WHERE workspace_id = ? AND design_gap_id = ?", {workspace_id, id});
        const auto uc = db.get(
            "SELECT COUNT(*) AS c\n"
            "       FROM utterances u\n"
            "       JOIN sessions s ON s.id = u.session_id AND s.workspace_id = u.workspace_id\n"
            "      WHERE u.workspace_id = ?\n"
            "        AND (s.scene = ? OR s.title = ?)",
            {workspace_id, "gap:" + id, "discussion-of-gap:" + id});
        const std::int64_t opinion_count = oc ? oc->i64("c") : 0;
        const std::int64_t utterance_count = uc ? uc->i64("c") : 0;
        total_opinions += opinion_count;
        total_utterances += utterance_count;
        Json j = Json::object();
        j["id"] = row["id"].to_json();
        j["title"] = row["title"].to_json();
        j["description"] = row["description"].to_json();
        j["status"] = row["status"].to_json();
        j["gapIn"] = row["gap_in"].to_json();
        j["expectedAffect"] = row["expected_affect"].to_json();
        j["observedAffect"] = row["observed_affect"].to_json();
        j["opinionCount"] = opinion_count;
        j["utteranceCount"] = utterance_count;
        const std::int64_t size = opinion_count * 3 + utterance_count;
        j["size"] = size;
        j["updatedAt"] = row["updated_at"].to_json();
        j["opinions"] = opinions;
        topics.push_back({std::move(j), static_cast<double>(size), row["updated_at"].as_double(0)});
    }
    // b.size - a.size || b.updatedAt - a.updatedAt (安定)
    std::stable_sort(topics.begin(), topics.end(), [](const Topic& a, const Topic& b) {
        if (a.size != b.size) return b.size < a.size;
        return b.updated_at < a.updated_at;
    });
    Json topics_json = Json::array();
    for (auto& t : topics) topics_json.push_back(std::move(t.json));
    Json out = Json::object();
    out["workspaceId"] = workspace_id;
    out["generatedAt"] = time::now_ms();
    out["topics"] = topics_json;
    out["totals"] = Json{{"topics", topics.size()}, {"opinions", total_opinions}, {"utterances", total_utterances}};
    return out;
}

} // namespace di::visualize
