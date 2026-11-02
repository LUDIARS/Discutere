// @implements DI-EXTERNAL-DISCUSSION
#include "external_discussion/contracts.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_set>

#include "base/strings.hpp"

namespace di::external_discussion {

namespace {

// ICU root 照合での ASCII の並び (node で確認: 空白・記号・数字・英字 (小文字が先))。
constexpr std::string_view kAsciiOrder =
    " _-,;:!?.'\"()[]{}@*/\\&#%`^+<=>|~$0123456789aAbBcCdDeEfFgGhHiIjJkKlLmMnNoOpPqQrRsStTuUvVwWxXyYzZ";

struct Weight {
    std::uint32_t primary;
    int tertiary;
};

std::vector<Weight> weights(const std::string& s) {
    std::vector<Weight> out;
    std::size_t i = 0;
    while (i < s.size()) {
        const std::uint32_t cp = str::next_codepoint(s, i);
        if (cp < 0x20 || cp == 0x7F) continue;  // 制御文字は無視される
        if (cp < 0x80) {
            const auto pos = kAsciiOrder.find(static_cast<char>(cp));
            const bool letter = (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z');
            if (letter) {
                const char lower = static_cast<char>(cp >= 'a' ? cp : cp + 32);
                out.push_back({static_cast<std::uint32_t>(kAsciiOrder.find(lower)), cp >= 'a' ? 0 : 1});
            } else {
                out.push_back({static_cast<std::uint32_t>(pos), 0});
            }
        } else {
            out.push_back({0x1000 + cp, 0});
        }
    }
    return out;
}

bool safe_integer(const Json& v, std::int64_t& out) {
    if (v.is_number_integer()) {
        const auto n = v.get<std::int64_t>();
        if (n < -9007199254740991LL || n > 9007199254740991LL) return false;
        out = n;
        return true;
    }
    if (v.is_number_unsigned()) {
        const auto n = v.get<std::uint64_t>();
        if (n > 9007199254740991ULL) return false;
        out = static_cast<std::int64_t>(n);
        return true;
    }
    if (v.is_number_float()) {
        const double d = v.get<double>();
        if (!std::isfinite(d) || std::trunc(d) != d || std::fabs(d) > 9007199254740991.0) return false;
        out = static_cast<std::int64_t>(d);
        return true;
    }
    return false;
}

/** /^actio:[a-zA-Z0-9:_-]{1,220}$/ */
bool valid_scene(const std::string& s) {
    constexpr std::string_view prefix = "actio:";
    if (!str::starts_with(s, prefix)) return false;
    const std::size_t n = s.size() - prefix.size();
    if (n < 1 || n > 220) return false;
    return std::all_of(s.begin() + static_cast<std::ptrdiff_t>(prefix.size()), s.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ':' || c == '_' || c == '-';
    });
}

}  // namespace

int locale_compare(const std::string& a, const std::string& b) {
    const auto wa = weights(a);
    const auto wb = weights(b);
    const std::size_t n = std::min(wa.size(), wb.size());
    for (std::size_t i = 0; i < n; ++i)
        if (wa[i].primary != wb[i].primary) return wa[i].primary < wb[i].primary ? -1 : 1;
    if (wa.size() != wb.size()) return wa.size() < wb.size() ? -1 : 1;
    for (std::size_t i = 0; i < n; ++i)
        if (wa[i].tertiary != wb[i].tertiary) return wa[i].tertiary < wb[i].tertiary ? -1 : 1;
    return 0;
}

ConversationInput parse_conversation(const Json& value) {
    if (!value.is_object()) throw ConversationError("conversation required");
    auto scene = value.find("scene");
    if (scene == value.end() || !scene->is_string() || !valid_scene(scene->get_ref<const std::string&>()))
        throw ConversationError("invalid scene");
    auto enabled = value.find("enabled");
    auto messages = value.find("messages");
    if (enabled == value.end() || !enabled->is_boolean() || messages == value.end() || !messages->is_array() ||
        messages->size() > 80)
        throw ConversationError("invalid conversation");
    std::unordered_set<std::string> seen;
    std::size_t bytes = 0;
    ConversationInput out;
    out.scene = scene->get<std::string>();
    out.enabled = enabled->get<bool>();
    for (const auto& item : *messages) {
        if (!item.is_object()) throw ConversationError("invalid message");
        auto id = item.find("id");
        auto kind = item.find("kind");
        auto text = item.find("text");
        auto at = item.find("at");
        std::int64_t at_value = 0;
        if (id == item.end() || !id->is_string() || id->get_ref<const std::string&>().empty() ||
            str::js_length(id->get_ref<const std::string&>()) > 200 || seen.count(id->get_ref<const std::string&>()) ||
            kind == item.end() || !kind->is_string() ||
            (kind->get_ref<const std::string&>() != "human" && kind->get_ref<const std::string&>() != "ai") ||
            text == item.end() || !text->is_string() || str::js_length(text->get_ref<const std::string&>()) > 8000 ||
            at == item.end() || !safe_integer(*at, at_value) || at_value < 0)
            throw ConversationError("invalid message");
        seen.insert(id->get<std::string>());
        bytes += str::js_length(text->get_ref<const std::string&>());
        out.messages.push_back({id->get<std::string>(), kind->get<std::string>(), text->get<std::string>(), at_value});
    }
    if (bytes > 32000) throw ConversationError("conversation too large");
    std::stable_sort(out.messages.begin(), out.messages.end(), [](const ConversationMessage& a, const ConversationMessage& b) {
        if (a.at != b.at) return a.at < b.at;
        return locale_compare(a.id, b.id) < 0;
    });
    return out;
}

Json to_json(const ConversationMessage& m) {
    Json o = Json::object();
    o["id"] = m.id;
    o["kind"] = m.kind;
    o["text"] = m.text;
    o["at"] = m.at;
    return o;
}

Json to_json(const ConversationInput& input) {
    Json msgs = Json::array();
    for (const auto& m : input.messages) msgs.push_back(to_json(m));
    Json o = Json::object();
    o["scene"] = input.scene;
    o["enabled"] = input.enabled;
    o["messages"] = std::move(msgs);
    return o;
}

Json to_json(const ParticipationResult& r) {
    Json o = Json::object();
    o["status"] = r.status;
    if (r.proposalId) o["proposalId"] = *r.proposalId;
    if (r.text) o["text"] = *r.text;
    if (r.stance) o["stance"] = *r.stance;
    if (r.sourceMessageId) o["sourceMessageId"] = *r.sourceMessageId;
    return o;
}

ParticipationResult participation_result_from_json(const Json& j) {
    ParticipationResult r;
    if (!j.is_object()) return r;
    r.status = json_string(j, "status");
    r.proposalId = json_opt_string(j, "proposalId");
    r.text = json_opt_string(j, "text");
    r.stance = json_opt_string(j, "stance");
    r.sourceMessageId = json_opt_string(j, "sourceMessageId");
    return r;
}

} // namespace di::external_discussion
