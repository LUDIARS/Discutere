#include "api/config_file_patch.hpp"

#include <mutex>
#include <stdexcept>

#include "base/env.hpp"
#include "base/fsutil.hpp"
#include "config.hpp"

namespace di::api {

std::string config_file_path() { return fs::resolve(env::get("DISCUTERE_CONFIG").value_or("./discutere.config.json")); }

void patch_config_file(const std::function<void(Json& current)>& mutate) {
    static std::mutex mutex;
    std::lock_guard lock(mutex);
    const std::string file = config_file_path();
    Json current = Json::object();
    if (fs::exists(file)) {
        const auto text = fs::read_file(file, config::kMaxConfigFileBytes);
        if (!text) throw std::runtime_error("config file is unreadable or too large");
        std::string error;
        auto parsed = parse_json(*text, JsonLimits{config::kMaxConfigFileBytes, 200'000, 64}, &error);
        if (!parsed) throw std::runtime_error("config file is not valid JSON: " + error);
        if (!parsed->is_object()) throw std::runtime_error("config file is not a JSON object");
        current = std::move(*parsed);
    }
    mutate(current);
    if (!fs::write_file_atomic(file, json_stringify_pretty(current, 2) + "\n")) throw std::runtime_error("config file write failed");
}

} // namespace di::api
