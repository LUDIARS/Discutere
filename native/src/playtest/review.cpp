// @implements SPEC-DI-AI-PLAYTEST-REVIEWS
#include "playtest/review.hpp"

#include <algorithm>
#include <initializer_list>
#include <utility>

#include "base/strings.hpp"
#include "base/time.hpp"
#include "playtest/contracts/runtime.hpp" /* augur-inject:import:c0412bb6 */
#include "contracts/review.hpp" /* augur-inject:contract-predicate:d8abd27c */

namespace di::playtest {
namespace {

bool has_exact_fields(const Json& value, std::initializer_list<const char*> names) {
    return value.is_object() && value.size() == names.size() &&
           std::all_of(names.begin(), names.end(), [&](const char* name) { return value.contains(name); });
}

bool valid_text(const Json& value, std::size_t limit) {
    if (!value.is_string()) return false;
    const auto& text = value.get_ref<const std::string&>();
    return text.find('\0') == std::string::npos && !str::trim(text).empty() && str::js_length(text) <= limit;
}

bool valid_numbers(const Json& values) {
    return values.is_array() && values.size() <= 100 && std::all_of(values.begin(), values.end(), [](const Json& value) {
        if (value.is_number_unsigned()) return value.get<std::uint64_t>() <= 9'007'199'254'740'991ULL;
        if (!value.is_number_integer()) return false;
        const auto number = value.get<std::int64_t>();
        return number >= 0 && number <= 9'007'199'254'740'991LL;
    });
}

bool valid_points(const Json& points, bool required) {
    if (!points.is_array() || (required && points.empty()) || points.size() > 8) return false;
    for (const auto& point : points) {
        if (!has_exact_fields(point, {"aspect", "text", "evidence"}) || !valid_text(point.at("text"), 300)) return false;
        const auto& aspect = point.at("aspect");
        const auto& evidence = point.at("evidence");
        if (aspect != "ux" && aspect != "control" && aspect != "game") return false;
        if (!has_exact_fields(evidence, {"ticks", "shots"}) || !valid_numbers(evidence.at("ticks")) || !valid_numbers(evidence.at("shots")))
            return false;
    }
    return true;
}

bool valid_played_at(const Json& value) {
    if (!value.is_string()) return false;
    const auto& s = value.get_ref<const std::string&>();
    if (s.size() != 24) return false;
    const auto ms = time::parse_iso(s);
    return ms && time::iso_utc(*ms) == s;
}

} // namespace

std::optional<AiReview> parse_ai_review(const Json& body, const std::string& key) { return ::augur::invoke_observed(::augur::ContractSpec{"C-5", "844de6ea", "src/playtest/review.cpp:57"}, di::playtest::checks::review, [&]() -> std::optional<AiReview> { /* augur-inject:contract-wrap:844de6ea */
    if (!has_exact_fields(body, {"origin", "source", "run_id", "game_id", "game_title", "summary", "good", "bad", "confusions",
                       "reflection", "played_at"})) return std::nullopt;
    if (body.at("origin") != "ai" || body.at("source") != "ai-autoplayer" || !valid_text(body.at("run_id"), 200) ||
        !valid_text(body.at("game_id"), 200) || !valid_text(body.at("game_title"), 200) || !valid_text(body.at("summary"), 400) ||
        !valid_played_at(body.at("played_at"))) return std::nullopt;
    const auto run_id = body.at("run_id").get<std::string>();
    if (run_id != key || !valid_points(body.at("good"), true) || !valid_points(body.at("bad"), true) ||
        !valid_points(body.at("confusions"), false)) return std::nullopt;
    const auto& reflection = body.at("reflection");
    if (!reflection.is_array() || reflection.empty() || reflection.size() > 8 ||
        !std::all_of(reflection.begin(), reflection.end(), [](const Json& line) { return valid_text(line, 300); })) return std::nullopt;
    return AiReview{run_id, body.at("game_id").get<std::string>(), body.at("game_title").get<std::string>(), body};
}, body, key); /* augur-inject:contract-end:844de6ea */ }

std::string format_ai_review(const Json& review) {
    std::string result = "- " + review.at("played_at").get<std::string>() + " " + review.at("summary").get<std::string>();
    for (const auto& [key, label] : {std::pair{"good", "良い点"}, {"bad", "悪い点"}, {"confusions", "迷った点"}}) {
        for (const auto& point : review.at(key))
            result += "\n  - " + std::string(label) + " (" + point.at("aspect").get<std::string>() + "): " + point.at("text").get<std::string>();
    }
    for (const auto& line : review.at("reflection")) result += "\n  - 振り返り: " + line.get<std::string>();
    return result;
}

} // namespace di::playtest
