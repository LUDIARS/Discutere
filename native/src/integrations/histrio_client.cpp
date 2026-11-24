#include "integrations/histrio_client.hpp"

#include <algorithm>
#include <array>
#include <set>

#include "base/json.hpp"
#include "base/strings.hpp"
#include "infra/http_client.hpp"
#include "playtest/contracts/runtime.hpp" /* augur-inject:import:6aea94e0 */
#include "contracts/histrio_profile.hpp" /* augur-inject:contract-predicate:a19a5f72 */

namespace di::integrations {
namespace {

constexpr std::size_t kMaxResponseBytes = 512u << 10;
constexpr std::size_t kMaxPatterns = 256;

bool valid_key(std::string_view key) {
    return !key.empty() && key.size() <= 128 && std::ranges::all_of(key, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '.' || c == '_' || c == '-';
    });
}

std::string prompt_line(const Json& body, std::string_view key, std::size_t max_bytes) {
    auto raw = json_string(body, key);
    if (raw.size() > max_bytes) return {};
    auto line = str::trim(str::join(str::split_lines(raw), " "));
    if (std::ranges::any_of(line, [](unsigned char c) { return c < 32 || c == 127; })) return {};
    return line;
}

std::optional<std::string> egogram_tone(const Json& egogram) {
    constexpr std::array<std::string_view, 5> axes{"CP", "NP", "A", "FC", "AC"};
    constexpr std::array<std::string_view, 5> tones{"歯切れよく", "相手を気遣って", "落ち着いて淡々と",
                                                 "率直で生き生きと", "控えめで丁寧に"};
    double highest = 0;
    std::optional<std::string> tone;
    for (std::size_t i = 0; i < axes.size(); ++i) {
        const auto score = json_opt_number(egogram, axes[i]);
        if (!score || *score < 0 || *score > 100) return std::nullopt;
        if (*score > highest) {
            highest = *score;
            tone = std::string(tones[i]);
        }
    }
    return tone;
}

std::optional<HistrioProfile> parse_histrio_profile(const Json& body, std::string_view persona_id) { return ::augur::invoke_observed(::augur::ContractSpec{"C-11", "afa59e55", "src/integrations/histrio_client.cpp:49"}, di::integrations::checks::histrio_profile, [&]() -> std::optional<HistrioProfile> { /* augur-inject:contract-wrap:afa59e55 */
    const auto id = json_string(body, "id");
    const auto ns = json_string(body, "namespace");
    auto pattern = json_string(body, "script_pattern_id");
    auto version = json_string(body, "script_pattern_version");
    if (id != persona_id || (ns != "di" && ns != "shared") || !valid_key(pattern) || !valid_key(version))
        return std::nullopt;
    return HistrioProfile{std::move(pattern), std::move(version), egogram_tone(json_object(body, "egogram"))};
}, body, persona_id); /* augur-inject:contract-end:afa59e55 */ }

std::optional<std::vector<HistrioScriptPattern>> parse_patterns(const Json& body) {
    if (!body.is_array() || body.empty() || body.size() > kMaxPatterns) return std::nullopt;
    std::vector<HistrioScriptPattern> patterns;
    std::set<std::string> ids;
    for (const auto& item : body) {
        const auto ns = json_string(item, "namespace");
        HistrioScriptPattern pattern{json_string(item, "id"), json_string(item, "version"),
                                     prompt_line(item, "label", 96), prompt_line(item, "healthy", 320),
                                     prompt_line(item, "stress", 320)};
        if ((ns != "di" && ns != "shared") || !valid_key(pattern.id) || !valid_key(pattern.version) ||
            pattern.label.empty() || pattern.healthy.empty() || pattern.stress.empty() || !ids.insert(pattern.id).second)
            return std::nullopt;
        patterns.push_back(std::move(pattern));
    }
    return patterns;
}

HistrioResult<Json> get_json(const std::string& base_url, const std::string& path) {
    if (base_url.empty()) return {};
    http::ClientRequest request;
    request.url = base_url + path + "?namespace=di";
    request.timeout = HistrioClient::kDeadline;
    request.connect_timeout = HistrioClient::kDeadline;
    request.max_response_bytes = kMaxResponseBytes;
    request.allow_private_network = true;  // HISTRIO_URL is configured by the operator.
    request.max_redirects = 0;
    const auto response = http::fetch(request);
    if (!response.transport_ok) {
        if (response.error_kind == http::FetchErrorKind::timeout) return {std::nullopt, "応答期限 (1.5 秒)"};
        if (response.error_kind == http::FetchErrorKind::too_large) return {std::nullopt, "応答サイズ超過"};
        return {std::nullopt, "接続失敗"};
    }
    if (response.status == 404) return {std::nullopt, "未登録 (404)"};
    if (response.status != 200) return {std::nullopt, "HTTP " + std::to_string(response.status)};
    auto json = parse_json(response.body, {kMaxResponseBytes, 32'768, 16});
    if (!json) return {std::nullopt, "不正な JSON"};
    return {std::move(json), {}};
}

} // namespace

HistrioClient::HistrioClient(std::string base_url) : base_url_(str::trim(base_url)) {
    while (base_url_.size() > 1 && base_url_.ends_with('/')) base_url_.pop_back();
}

HistrioResult<HistrioProfile> HistrioClient::profile(std::string_view persona_id) const {
    if (!enabled()) return {};
    if (!valid_key(persona_id) || persona_id == "." || persona_id == "..") return {std::nullopt, "不正な人物 ID"};
    const auto reply = get_json(base_url_, "/v1/personas/" + std::string(persona_id) + "/profile");
    if (!reply.value) return {std::nullopt, reply.error};
    auto profile = parse_histrio_profile(*reply.value, persona_id);
    if (!profile) return {std::nullopt, "不正なプロフィール形式"};
    return {std::move(profile), {}};
}

HistrioResult<std::vector<HistrioScriptPattern>> HistrioClient::script_patterns() const {
    const auto reply = get_json(base_url_, "/v1/script-patterns");
    if (!reply.value) return {std::nullopt, reply.error};
    auto patterns = parse_patterns(*reply.value);
    if (!patterns) return {std::nullopt, "不正な脚本カタログ形式"};
    return {std::move(patterns), {}};
}

} // namespace di::integrations
