#include "crawler/sources/types.hpp"

#include <chrono>
#include <cmath>
#include <limits>
#include <semaphore>
#include <thread>

#include "base/time.hpp"

namespace di::crawler::sources {

bool is_external_source(std::string_view s) {
    return s == "steam" || s == "youtube" || s == "youtube-livechat" || s == "website" || s == "reddit" || s == "niconico" ||
           s == "fandom" || s == "opencritic" || s == "feedback" || s == "glab";
}

Json to_json(const ExternalSignal& v) {
    Json o = Json::object();
    if (v.votedUp) o["votedUp"] = *v.votedUp;
    if (v.upvotes) o["upvotes"] = *v.upvotes;
    return o;
}

namespace {
Json number(double v) {
    if (std::isnan(v) || std::isinf(v)) return nullptr;  // JSON.stringify(NaN) → null
    if (v == std::floor(v) && std::fabs(v) < 9.007199254740992e15) return static_cast<std::int64_t>(v);
    return v;
}
} // namespace

Json to_json(const ExternalUtterance& v) {
    Json o = Json::object();
    o["source"] = v.source;
    o["nativeId"] = v.nativeId;
    o["gameSlug"] = v.gameSlug;
    o["threadKey"] = v.threadKey;
    o["content"] = v.content;
    if (v.summary) o["summary"] = *v.summary;
    if (v.lang) o["lang"] = *v.lang;
    o["postedAt"] = number(v.postedAt);
    if (v.videoOffsetMs) o["videoOffsetMs"] = number(*v.videoOffsetMs);
    o["authorId"] = v.authorId;
    if (v.authorName) o["authorName"] = *v.authorName;
    if (v.parentNativeId) o["parentNativeId"] = *v.parentNativeId;
    if (v.signal) o["signal"] = to_json(*v.signal);
    o["sourceUrl"] = v.sourceUrl;
    return o;
}

std::optional<ExternalUtterance> utterance_from_json(const Json& j) {
    if (!j.is_object()) return std::nullopt;
    for (const char* k : {"source", "nativeId", "gameSlug", "threadKey", "content", "authorId", "sourceUrl"})
        if (!j.contains(k) || !j[k].is_string()) return std::nullopt;
    ExternalUtterance u;
    u.source = j["source"].get<std::string>();
    u.nativeId = j["nativeId"].get<std::string>();
    u.gameSlug = j["gameSlug"].get<std::string>();
    u.threadKey = j["threadKey"].get<std::string>();
    u.content = j["content"].get<std::string>();
    u.summary = json_opt_string(j, "summary");
    u.lang = json_opt_string(j, "lang");
    u.postedAt = json_opt_number(j, "postedAt").value_or(std::numeric_limits<double>::quiet_NaN());
    u.videoOffsetMs = json_opt_number(j, "videoOffsetMs");
    u.authorId = j["authorId"].get<std::string>();
    u.authorName = json_opt_string(j, "authorName");
    u.parentNativeId = json_opt_string(j, "parentNativeId");
    if (j.contains("signal") && j["signal"].is_object()) {
        ExternalSignal s;
        s.votedUp = json_opt_bool(j["signal"], "votedUp");
        s.upvotes = json_opt_number(j["signal"], "upvotes");
        u.signal = s;
    }
    u.sourceUrl = j["sourceUrl"].get<std::string>();
    return u;
}

namespace {
std::counting_semaphore<64>& crawler_slots() {
    static std::counting_semaphore<64> slots(static_cast<std::ptrdiff_t>(kCrawlerMaxParallel));
    return slots;
}
} // namespace

http::ClientResponse do_fetch(const FetchFn& fetch_impl, const http::ClientRequest& request) {
    // 外部取得の同時実行を上限で絞る (待ちは接続期限まで。待てなければ失敗)。
    if (!crawler_slots().try_acquire_for(request.connect_timeout)) {
        http::ClientResponse busy;
        busy.error = "busy: too many concurrent crawler requests";
        return busy;
    }
    struct Release {
        ~Release() { crawler_slots().release(); }
    } release;
    return fetch_impl ? fetch_impl(request) : http::fetch(request);
}

double js_date_parse(std::string_view text) {
    auto ms = time::parse_iso(text);
    if (!ms) return std::numeric_limits<double>::quiet_NaN();
    return static_cast<double>(*ms);
}

void polite_sleep(long long ms) {
    if (ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

} // namespace di::crawler::sources
