#include "flow/foresight/state.hpp"

#include <map>
#include <mutex>

namespace di::flow::foresight {
namespace {

std::mutex& state_mutex() {
    static std::mutex m;
    return m;
}
std::map<std::string, ForesightStatus>& statuses() {
    static std::map<std::string, ForesightStatus> s;
    return s;
}

} // namespace

Json to_json(const ForesightStatus& s) {
    Json j = Json::object();
    j["state"] = s.state;
    if (s.detail) j["detail"] = *s.detail;
    return j;
}

void set_foresight_status(const std::string& session_id, const ForesightState& state, const std::string& detail) {
    std::lock_guard lock(state_mutex());
    ForesightStatus s;
    s.state = state;
    if (!detail.empty()) s.detail = detail;
    statuses()[session_id] = std::move(s);
}

std::optional<ForesightStatus> get_foresight_status(const std::string& session_id) {
    std::lock_guard lock(state_mutex());
    auto it = statuses().find(session_id);
    if (it == statuses().end()) return std::nullopt;
    return it->second;
}

bool is_foresight_active(const std::string& session_id) {
    auto s = get_foresight_status(session_id);
    return s && (s->state == "pending" || s->state == "running");
}

void reset_foresight_status_for_test() {
    std::lock_guard lock(state_mutex());
    statuses().clear();
}

} // namespace di::flow::foresight
