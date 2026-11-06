#include "secrets/youtube_env.hpp"

#include "base/env.hpp"
#include "base/strings.hpp"

namespace di::secrets {
namespace {

constexpr const char* kKey = "DISCUTERE_YOUTUBE_API_KEY";

std::optional<std::string> from_value(const std::optional<std::string>& raw) {
    if (!raw) return std::nullopt;
    std::string trimmed = str::trim(*raw);
    if (trimmed.empty()) return std::nullopt;
    return trimmed;
}

} // namespace

std::optional<std::string> get_youtube_api_key() { return from_value(env::get(kKey)); }

std::optional<std::string> get_youtube_api_key(const EnvMap& env) {
    auto it = env.find(kKey);
    return from_value(it == env.end() ? std::nullopt : std::optional<std::string>(it->second));
}

YoutubeApiKeyStatus get_youtube_api_key_status() { return {get_youtube_api_key().has_value()}; }
YoutubeApiKeyStatus get_youtube_api_key_status(const EnvMap& env) { return {get_youtube_api_key(env).has_value()}; }

Json to_json(const YoutubeApiKeyStatus& status) { return Json{{"present", status.present}}; }

} // namespace di::secrets
