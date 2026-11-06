#include "discord_hook/discord_api.hpp"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <semaphore>
#include <thread>

#include "base/crypto.hpp"
#include "base/log.hpp"
#include "base/strings.hpp"
#include "base/url.hpp"

namespace di::discord_hook {

bool is_thread_type(int type) {
    return type == channel_type::AnnouncementThread || type == channel_type::PublicThread || type == channel_type::PrivateThread;
}

// ─── JSON 変換 ─────────────────────────────────────────────────

namespace {

std::string req_str(const Json& j, const char* key, const char* what) {
    if (!j.is_object() || !j.contains(key) || !j[key].is_string())
        throw std::runtime_error(std::string("discord: invalid ") + what + " (" + key + ")");
    return j[key].get<std::string>();
}

std::optional<int> opt_int(const Json& j, const char* key) {
    auto it = j.find(key);
    if (it != j.end() && it->is_number_integer()) return it->get<int>();
    return std::nullopt;
}

void put(Json& o, const char* key, const std::optional<std::string>& v) {
    if (v) o[key] = *v;
}

} // namespace

User user_from_json(const Json& j) {
    User u;
    u.id = req_str(j, "id", "user");
    u.username = json_string(j, "username");
    u.global_name = json_opt_string(j, "global_name");
    u.bot = json_bool(j, "bot");
    return u;
}

Json to_json(const User& v) {
    Json o{{"id", v.id}, {"username", v.username}};
    put(o, "global_name", v.global_name);
    if (v.bot) o["bot"] = true;
    return o;
}

ForumTag forum_tag_from_json(const Json& j) {
    ForumTag t;
    if (!j.is_object()) throw std::runtime_error("discord: invalid forum tag");
    t.id = json_opt_string(j, "id");
    t.name = json_string(j, "name");
    t.moderated = json_opt_bool(j, "moderated");
    t.emoji_id = json_opt_string(j, "emoji_id");
    t.emoji_name = json_opt_string(j, "emoji_name");
    return t;
}

Json to_json(const ForumTag& v) {
    Json o = Json::object();
    put(o, "id", v.id);
    o["name"] = v.name;
    if (v.moderated) o["moderated"] = *v.moderated;
    put(o, "emoji_id", v.emoji_id);
    put(o, "emoji_name", v.emoji_name);
    return o;
}

Channel channel_from_json(const Json& j) {
    Channel c;
    c.id = req_str(j, "id", "channel");
    c.type = opt_int(j, "type").value_or(channel_type::GuildText);
    c.guild_id = json_opt_string(j, "guild_id");
    c.name = json_opt_string(j, "name");
    c.parent_id = json_opt_string(j, "parent_id");
    c.topic = json_opt_string(j, "topic");
    c.position = opt_int(j, "position");
    c.owner_id = json_opt_string(j, "owner_id");
    c.last_message_id = json_opt_string(j, "last_message_id");
    for (const auto& t : json_array(j, "available_tags")) c.available_tags.push_back(forum_tag_from_json(t));
    for (const auto& t : json_array(j, "applied_tags"))
        if (t.is_string()) c.applied_tags.push_back(t.get<std::string>());
    if (j.contains("thread_metadata") && j["thread_metadata"].is_object()) {
        const Json& m = j["thread_metadata"];
        c.thread_metadata = ThreadMetadata{json_bool(m, "archived"), json_bool(m, "locked"), opt_int(m, "auto_archive_duration")};
    }
    return c;
}

Json to_json(const Channel& v) {
    Json o{{"id", v.id}, {"type", v.type}};
    put(o, "guild_id", v.guild_id);
    put(o, "name", v.name);
    if (v.parent_id) o["parent_id"] = *v.parent_id;
    put(o, "topic", v.topic);
    if (v.position) o["position"] = *v.position;
    put(o, "owner_id", v.owner_id);
    put(o, "last_message_id", v.last_message_id);
    if (!v.available_tags.empty()) {
        o["available_tags"] = Json::array();
        for (const auto& t : v.available_tags) o["available_tags"].push_back(to_json(t));
    }
    if (!v.applied_tags.empty()) o["applied_tags"] = v.applied_tags;
    if (v.thread_metadata) {
        Json m{{"archived", v.thread_metadata->archived}, {"locked", v.thread_metadata->locked}};
        if (v.thread_metadata->auto_archive_duration) m["auto_archive_duration"] = *v.thread_metadata->auto_archive_duration;
        o["thread_metadata"] = m;
    }
    return o;
}

std::uint64_t parse_permissions(const Json& value) {
    if (value.is_number_unsigned()) return value.get<std::uint64_t>();
    if (value.is_number_integer()) return static_cast<std::uint64_t>(std::max<std::int64_t>(0, value.get<std::int64_t>()));
    if (!value.is_string()) return 0;
    const auto& s = value.get_ref<const std::string&>();
    if (s.empty() || s.size() > 20) return 0;
    std::uint64_t out = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return 0;
        const std::uint64_t d = static_cast<std::uint64_t>(c - '0');
        if (out > (~0ull - d) / 10) return 0;  // 桁あふれ
        out = out * 10 + d;
    }
    return out;
}

Role role_from_json(const Json& j) {
    Role r;
    r.id = req_str(j, "id", "role");
    r.name = json_string(j, "name");
    r.permissions = j.contains("permissions") ? parse_permissions(j["permissions"]) : 0;
    r.position = opt_int(j, "position").value_or(0);
    return r;
}

Guild guild_from_json(const Json& j) {
    Guild g;
    g.id = req_str(j, "id", "guild");
    g.name = json_string(j, "name");
    g.owner_id = json_opt_string(j, "owner_id");
    for (const auto& r : json_array(j, "roles")) g.roles.push_back(role_from_json(r));
    return g;
}

Member member_from_json(const Json& j) {
    if (!j.is_object()) throw std::runtime_error("discord: invalid member");
    Member m;
    if (j.contains("user") && j["user"].is_object()) m.user = user_from_json(j["user"]);
    m.nick = json_opt_string(j, "nick");
    for (const auto& r : json_array(j, "roles"))
        if (r.is_string()) m.roles.push_back(r.get<std::string>());
    return m;
}

Message message_from_json(const Json& j) {
    Message m;
    m.id = req_str(j, "id", "message");
    m.channel_id = req_str(j, "channel_id", "message");
    m.guild_id = json_opt_string(j, "guild_id");
    if (j.contains("author") && j["author"].is_object()) m.author = user_from_json(j["author"]);
    m.content = json_string(j, "content");
    m.timestamp = json_string(j, "timestamp");
    for (const auto& u : json_array(j, "mentions"))
        if (u.is_object() && u.contains("id") && u["id"].is_string()) m.mentions.push_back(user_from_json(u));
    if (j.contains("message_reference") && j["message_reference"].is_object()) {
        const Json& r = j["message_reference"];
        m.message_reference =
            MessageReference{json_opt_string(r, "message_id"), json_opt_string(r, "channel_id"), json_opt_string(r, "guild_id")};
    }
    m.webhook_id = json_opt_string(j, "webhook_id");
    for (const auto& a : json_array(j, "attachments")) {
        if (!a.is_object() || !a.contains("id") || !a["id"].is_string()) continue;
        m.attachments.push_back(Attachment{a["id"].get<std::string>(), json_string(a, "filename"), json_string(a, "url"),
                                           json_int(a, "size"), json_opt_string(a, "content_type")});
    }
    m.type = opt_int(j, "type").value_or(0);
    return m;
}

Json to_json(const Message& v) {
    Json o{{"id", v.id}, {"channel_id", v.channel_id}};
    put(o, "guild_id", v.guild_id);
    o["author"] = to_json(v.author);
    o["content"] = v.content;
    o["timestamp"] = v.timestamp;
    o["mentions"] = Json::array();
    for (const auto& u : v.mentions) o["mentions"].push_back(to_json(u));
    if (v.message_reference) {
        Json r = Json::object();
        put(r, "message_id", v.message_reference->message_id);
        put(r, "channel_id", v.message_reference->channel_id);
        put(r, "guild_id", v.message_reference->guild_id);
        o["message_reference"] = r;
    }
    put(o, "webhook_id", v.webhook_id);
    o["type"] = v.type;
    return o;
}

Webhook webhook_from_json(const Json& j) {
    Webhook w;
    w.id = req_str(j, "id", "webhook");
    w.type = opt_int(j, "type").value_or(1);
    w.name = json_opt_string(j, "name");
    w.token = json_opt_string(j, "token");
    w.channel_id = json_opt_string(j, "channel_id");
    w.guild_id = json_opt_string(j, "guild_id");
    return w;
}

GatewayBot gateway_bot_from_json(const Json& j) {
    GatewayBot g;
    g.url = req_str(j, "url", "gateway");
    g.shards = opt_int(j, "shards").value_or(1);
    const Json& limit = json_object(j, "session_start_limit");
    g.session_start_total = opt_int(limit, "total").value_or(0);
    g.session_start_remaining = opt_int(limit, "remaining").value_or(0);
    g.session_start_reset_after = json_int(limit, "reset_after");
    g.max_concurrency = opt_int(limit, "max_concurrency").value_or(1);
    return g;
}

Json allowed_mentions_none() { return Json{{"parse", Json::array()}}; }

// ─── ルートの鍵と伏せ字 ──────────────────────────────────────────

namespace {

std::string_view strip_query(std::string_view path) {
    const auto q = path.find('?');
    return q == std::string_view::npos ? path : path.substr(0, q);
}

bool all_digits(std::string_view s) {
    if (s.empty()) return false;
    for (char c : s)
        if (c < '0' || c > '9') return false;
    return true;
}

/** mode: 0 = レート制限の鍵, 1 = ログ用。 */
std::string normalize_route(std::string_view method, std::string_view path, int mode) {
    const auto parts = str::split(strip_query(path), "/");
    std::vector<std::string> out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        const std::string& seg = parts[i];
        const std::string prev = i > 0 ? parts[i - 1] : std::string();
        const std::string prev2 = i > 1 ? parts[i - 2] : std::string();
        // webhooks/{id}/{token} と interactions/{id}/{token} の token は秘密なので常に伏せる。
        if ((prev2 == "webhooks" || prev2 == "interactions") && i >= 2 && !seg.empty()) {
            out.push_back(":token");
            continue;
        }
        if (mode == 0) {
            const bool major = prev == "channels" || prev == "guilds" || prev == "webhooks";
            if (prev == "reactions") {
                out.push_back(":reaction");  // 絵文字ごとに別バケットにしない (discord.js と同じ)
                continue;
            }
            if (!major && all_digits(seg)) {
                out.push_back(":id");
                continue;
            }
        }
        out.push_back(seg);
    }
    return str::to_upper(method) + " " + str::join(out, "/");
}

} // namespace

std::string rate_limit_route_key(std::string_view method, std::string_view path) { return normalize_route(method, path, 0); }

std::string redact_route(std::string_view method, std::string_view path) { return normalize_route(method, path, 1); }

std::uint64_t compute_base_permissions(const Guild& guild, const Member& member, const std::string& user_id) {
    if (guild.owner_id && *guild.owner_id == user_id) return permission::All;
    std::uint64_t perms = 0;
    for (const auto& r : guild.roles)
        if (r.id == guild.id) perms |= r.permissions;  // @everyone
    for (const auto& id : member.roles)
        for (const auto& r : guild.roles)
            if (r.id == id) perms |= r.permissions;
    if (perms & permission::Administrator) return permission::All;
    return perms;
}

// ─── クライアント本体 ─────────────────────────────────────────────

struct DiscordApi::Impl {
    struct Bucket {
        std::mutex queue;  // 同じバケットへの要求を 1 本ずつ送る
        int remaining = 1;
        std::chrono::steady_clock::time_point reset_at{};
    };

    explicit Impl(std::size_t parallel) : slots(static_cast<std::ptrdiff_t>(std::clamp<std::size_t>(parallel, 1, 64))) {}

    std::mutex mutex;  // 下の表を守る
    std::condition_variable cv;
    bool stopping = false;
    std::map<std::string, std::string> route_to_bucket;  // ルート鍵 → バケット id (hash + major)
    std::map<std::string, std::shared_ptr<Bucket>> buckets;
    std::chrono::steady_clock::time_point global_reset_at{};
    std::optional<User> me;
    std::counting_semaphore<64> slots;

    std::shared_ptr<Bucket> bucket_for(const std::string& route_key) {
        std::lock_guard lock(mutex);
        auto it = route_to_bucket.find(route_key);
        const std::string id = it == route_to_bucket.end() ? route_key : it->second;
        auto& b = buckets[id];
        if (!b) b = std::make_shared<Bucket>();
        return b;
    }

    /** 応答のバケット hash をルートに結び付ける。 */
    std::shared_ptr<Bucket> learn_bucket(const std::string& route_key, const std::string& major, const std::string& hash,
                                         const std::shared_ptr<Bucket>& current) {
        std::lock_guard lock(mutex);
        const std::string id = hash + ":" + major;
        route_to_bucket[route_key] = id;
        auto& b = buckets[id];
        if (!b) b = current;  // 今のバケット状態をそのまま引き継ぐ
        return b;
    }

    /** 停止要求で起きたら false。 */
    bool sleep_for(std::chrono::milliseconds d) {
        std::unique_lock lock(mutex);
        return !cv.wait_for(lock, d, [&] { return stopping; });
    }
};

namespace {

std::string major_of(std::string_view path) {
    const auto parts = str::split(strip_query(path), "/");
    for (std::size_t i = 0; i + 1 < parts.size(); ++i)
        if (parts[i] == "channels" || parts[i] == "guilds" || parts[i] == "webhooks") return parts[i] + "/" + parts[i + 1];
    return "global";
}

bool idempotent(std::string_view method) { return method != "POST"; }

/** "1.5" のような秒数を ms にする。 */
std::optional<std::chrono::milliseconds> seconds_header(const http::Headers& h, std::string_view name) {
    auto v = h.get(name);
    if (!v) return std::nullopt;
    auto d = str::parse_double(str::trim(*v));
    if (!d || !std::isfinite(*d) || *d < 0) return std::nullopt;
    return std::chrono::milliseconds(static_cast<long long>(std::ceil(*d * 1000.0)));
}

std::string multipart_boundary() { return "----DiscutereBoundary" + crypto::hex_encode(crypto::random_bytes(12)); }

std::string build_multipart(const std::string& boundary, const Json& payload, const std::vector<FileUpload>& files) {
    Json body = payload.is_object() ? payload : Json::object();
    if (!body.contains("attachments")) {
        Json attachments = Json::array();
        for (std::size_t i = 0; i < files.size(); ++i) attachments.push_back(Json{{"id", i}, {"filename", files[i].name}});
        body["attachments"] = attachments;
    }
    auto safe_name = [](const std::string& n) {
        std::string out;
        for (char c : n) out.push_back((c == '"' || c == '\r' || c == '\n' || c == '\\') ? '_' : c);
        return out;
    };
    std::string out;
    out += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"payload_json\"\r\nContent-Type: application/json\r\n\r\n";
    out += json_stringify(body) + "\r\n";
    for (std::size_t i = 0; i < files.size(); ++i) {
        out += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"files[" + std::to_string(i) + "]\"; filename=\"" +
               safe_name(files[i].name) + "\"\r\nContent-Type: " + files[i].content_type + "\r\n\r\n";
        out += files[i].data + "\r\n";
    }
    out += "--" + boundary + "--\r\n";
    return out;
}

} // namespace

DiscordApi::DiscordApi(DiscordApiOptions options) : options_(std::move(options)), impl_(std::make_unique<Impl>(options_.max_parallel)) {
    while (!options_.base_url.empty() && options_.base_url.back() == '/') options_.base_url.pop_back();
}

DiscordApi::~DiscordApi() { shutdown(); }

void DiscordApi::shutdown() {
    {
        std::lock_guard lock(impl_->mutex);
        impl_->stopping = true;
    }
    impl_->cv.notify_all();
}

Json DiscordApi::request(std::string_view method_in, std::string_view path, const std::optional<Json>& body, const RequestOptions& options) {
    const std::string method = str::to_upper(method_in);
    const std::string route = redact_route(method, path);
    const std::string route_key = rate_limit_route_key(method, path);
    const std::string major = major_of(path);
    auto fail = [&](const std::string& what, int status, int code, bool rate_limited) -> DiscordApiError {
        return DiscordApiError("discord " + route + " " + what, status, code, route, rate_limited);
    };

    std::string target = options_.base_url + std::string(path);
    if (!options.query.empty()) target += (str::contains(path, "?") ? "&" : "?") + url::build_query(options.query);

    http::ClientRequest req;
    req.method = method;
    req.url = target;
    req.timeout = options_.timeout;
    req.connect_timeout = options_.connect_timeout;
    req.max_response_bytes = options_.max_response_bytes;
    req.allow_private_network = options_.allow_private_network;
    req.max_redirects = 0;
    req.headers.set("User-Agent", options_.user_agent);
    if (options.auth) req.headers.set("Authorization", "Bot " + options_.token);
    if (options.reason) req.headers.set("X-Audit-Log-Reason", url::encode_component(*options.reason));
    if (!options.files.empty()) {
        const std::string boundary = multipart_boundary();
        req.headers.set("Content-Type", "multipart/form-data; boundary=" + boundary);
        req.body = build_multipart(boundary, body.value_or(Json::object()), options.files);
    } else if (body) {
        req.headers.set("Content-Type", "application/json");
        req.body = json_stringify(*body);
    }

    const int max_retries = std::max(0, options_.max_retries);
    for (int attempt = 0;; ++attempt) {
        auto bucket = impl_->bucket_for(route_key);
        std::unique_lock queue(bucket->queue);

        // 全体 (global) の制限の待ち
        {
            std::chrono::milliseconds wait{0};
            {
                std::lock_guard lock(impl_->mutex);
                if (impl_->stopping) throw fail("aborted (shutdown)", 0, 0, false);
                const auto now = std::chrono::steady_clock::now();
                if (impl_->global_reset_at > now)
                    wait = std::chrono::duration_cast<std::chrono::milliseconds>(impl_->global_reset_at - now) + std::chrono::milliseconds(1);
            }
            if (wait > options_.max_rate_limit_wait) throw fail("global rate limit wait exceeds limit", 429, 0, true);
            if (wait.count() > 0 && !impl_->sleep_for(wait)) throw fail("aborted (shutdown)", 0, 0, false);
        }
        // バケットの残りが 0 ならリセットまで待つ
        {
            const auto now = std::chrono::steady_clock::now();
            if (bucket->remaining <= 0 && bucket->reset_at > now) {
                const auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(bucket->reset_at - now) + std::chrono::milliseconds(1);
                if (wait > options_.max_rate_limit_wait) throw fail("rate limit wait exceeds limit", 429, 0, true);
                if (!impl_->sleep_for(wait)) throw fail("aborted (shutdown)", 0, 0, false);
            }
        }

        if (!impl_->slots.try_acquire_for(options_.connect_timeout)) throw fail("busy: too many concurrent requests", 0, 0, false);
        http::ClientResponse res;
        try {
            res = http::fetch(req);
        } catch (...) {
            impl_->slots.release();
            throw;
        }
        impl_->slots.release();

        if (!res.transport_ok) {
            if (idempotent(method) && attempt < max_retries) {
                queue.unlock();
                if (!impl_->sleep_for(std::chrono::milliseconds(250 * (attempt + 1)))) throw fail("aborted (shutdown)", 0, 0, false);
                continue;
            }
            throw fail("request failed: " + res.error, 0, 0, false);
        }

        // バケットの状態を更新する
        if (auto hash = res.headers.get("X-RateLimit-Bucket"); hash && !hash->empty()) bucket = impl_->learn_bucket(route_key, major, *hash, bucket);
        if (auto rem = res.headers.get("X-RateLimit-Remaining")) {
            if (auto n = str::parse_int(str::trim(*rem))) bucket->remaining = static_cast<int>(std::clamp<std::int64_t>(*n, 0, 1'000'000));
        }
        if (auto after = seconds_header(res.headers, "X-RateLimit-Reset-After")) bucket->reset_at = std::chrono::steady_clock::now() + *after;

        if (res.status == 429) {
            // retry_after (秒, 小数) は本文の JSON か Retry-After ヘッダ
            JsonLimits small{64 * 1024, 1000, 8};
            auto parsed = parse_json(res.body, small);
            std::optional<std::chrono::milliseconds> retry_after;
            bool global = false;
            if (parsed && parsed->is_object()) {
                if (auto ra = json_opt_number(*parsed, "retry_after"); ra && std::isfinite(*ra) && *ra >= 0)
                    retry_after = std::chrono::milliseconds(static_cast<long long>(std::ceil(*ra * 1000.0)));
                global = json_bool(*parsed, "global");
            }
            if (!retry_after) retry_after = seconds_header(res.headers, "Retry-After");
            if (auto g = res.headers.get("X-RateLimit-Global"); g && str::iequals_ascii(str::trim(*g), "true")) global = true;
            const auto wait = retry_after.value_or(std::chrono::milliseconds(1000));
            if (global) {
                std::lock_guard lock(impl_->mutex);
                impl_->global_reset_at = std::chrono::steady_clock::now() + wait;
            } else {
                bucket->remaining = 0;
                bucket->reset_at = std::chrono::steady_clock::now() + wait;
            }
            log::warn("  discord: rate limited on " + route + " (retry after " + std::to_string(wait.count()) + "ms" +
                      (global ? ", global" : "") + ")");
            if (wait > options_.max_rate_limit_wait) throw fail("429: rate limit wait exceeds limit", 429, 0, true);
            if (attempt >= max_retries) throw fail("429: rate limited (retries exhausted)", 429, 0, true);
            continue;  // 次の周回で待ってから再試行する
        }

        if (res.status >= 500) {
            if (idempotent(method) && attempt < max_retries) {
                queue.unlock();
                if (!impl_->sleep_for(std::chrono::milliseconds(250 * (attempt + 1)))) throw fail("aborted (shutdown)", 0, 0, false);
                continue;
            }
            throw fail(std::to_string(res.status) + ": " + str::js_slice(res.body, 0, 200), res.status, 0, false);
        }

        if (res.status < 200 || res.status >= 300) {
            int code = 0;
            JsonLimits small{64 * 1024, 10'000, 16};
            if (auto parsed = parse_json(res.body, small); parsed && parsed->is_object()) code = static_cast<int>(json_int(*parsed, "code"));
            throw fail(std::to_string(res.status) + ": " + str::js_slice(res.body, 0, 200), res.status, code, false);
        }

        if (res.status == 204 || str::trim(res.body).empty()) return Json(nullptr);
        std::string error;
        auto parsed = parse_json(res.body, options_.json_limits, &error);
        if (!parsed) throw fail(std::to_string(res.status) + ": invalid JSON response (" + error + ")", res.status, 0, false);
        return std::move(*parsed);
    }
}

namespace {
std::string enc(const std::string& s) { return url::encode_component(s); }

template <class T, class F>
T convert(const Json& j, const std::string& route, F&& fn) {
    try {
        return fn(j);
    } catch (const std::exception& e) {
        throw DiscordApiError("discord " + route + " malformed response: " + e.what(), 200, 0, route, false);
    }
}
} // namespace

User DiscordApi::get_current_user() {
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->me) return *impl_->me;
    }
    auto me = convert<User>(request("GET", "/users/@me"), "GET /users/@me", user_from_json);
    std::lock_guard lock(impl_->mutex);
    impl_->me = me;
    return me;
}

Guild DiscordApi::get_guild(const std::string& guild_id) {
    const std::string path = "/guilds/" + enc(guild_id);
    return convert<Guild>(request("GET", path), redact_route("GET", path), guild_from_json);
}

std::vector<Channel> DiscordApi::get_guild_channels(const std::string& guild_id) {
    const std::string path = "/guilds/" + enc(guild_id) + "/channels";
    return convert<std::vector<Channel>>(request("GET", path), redact_route("GET", path), [](const Json& j) {
        if (!j.is_array()) throw std::runtime_error("expected array");
        std::vector<Channel> out;
        for (const auto& c : j) out.push_back(channel_from_json(c));
        return out;
    });
}

Channel DiscordApi::get_channel(const std::string& channel_id) {
    const std::string path = "/channels/" + enc(channel_id);
    return convert<Channel>(request("GET", path), redact_route("GET", path), channel_from_json);
}

Channel DiscordApi::create_guild_channel(const std::string& guild_id, const Json& body, const std::optional<std::string>& reason) {
    const std::string path = "/guilds/" + enc(guild_id) + "/channels";
    RequestOptions o;
    o.reason = reason;
    return convert<Channel>(request("POST", path, body, o), redact_route("POST", path), channel_from_json);
}

Channel DiscordApi::modify_channel(const std::string& channel_id, const Json& patch, const std::optional<std::string>& reason) {
    const std::string path = "/channels/" + enc(channel_id);
    RequestOptions o;
    o.reason = reason;
    return convert<Channel>(request("PATCH", path, patch, o), redact_route("PATCH", path), channel_from_json);
}

Member DiscordApi::get_guild_member(const std::string& guild_id, const std::string& user_id) {
    const std::string path = "/guilds/" + enc(guild_id) + "/members/" + enc(user_id);
    return convert<Member>(request("GET", path), redact_route("GET", path), member_from_json);
}

Message DiscordApi::get_message(const std::string& channel_id, const std::string& message_id) {
    const std::string path = "/channels/" + enc(channel_id) + "/messages/" + enc(message_id);
    return convert<Message>(request("GET", path), redact_route("GET", path), message_from_json);
}

Message DiscordApi::create_message(const std::string& channel_id, const Json& body, const std::vector<FileUpload>& files) {
    const std::string path = "/channels/" + enc(channel_id) + "/messages";
    RequestOptions o;
    o.files = files;
    return convert<Message>(request("POST", path, body, o), redact_route("POST", path), message_from_json);
}

Message DiscordApi::edit_message(const std::string& channel_id, const std::string& message_id, const Json& body) {
    const std::string path = "/channels/" + enc(channel_id) + "/messages/" + enc(message_id);
    return convert<Message>(request("PATCH", path, body), redact_route("PATCH", path), message_from_json);
}

void DiscordApi::create_reaction(const std::string& channel_id, const std::string& message_id, const std::string& emoji) {
    request("PUT", "/channels/" + enc(channel_id) + "/messages/" + enc(message_id) + "/reactions/" + enc(emoji) + "/@me");
}

std::vector<Webhook> DiscordApi::get_channel_webhooks(const std::string& channel_id) {
    const std::string path = "/channels/" + enc(channel_id) + "/webhooks";
    return convert<std::vector<Webhook>>(request("GET", path), redact_route("GET", path), [](const Json& j) {
        if (!j.is_array()) throw std::runtime_error("expected array");
        std::vector<Webhook> out;
        for (const auto& w : j) out.push_back(webhook_from_json(w));
        return out;
    });
}

Webhook DiscordApi::create_webhook(const std::string& channel_id, const std::string& name) {
    const std::string path = "/channels/" + enc(channel_id) + "/webhooks";
    return convert<Webhook>(request("POST", path, Json{{"name", name}}), redact_route("POST", path), webhook_from_json);
}

std::optional<Message> DiscordApi::execute_webhook(const std::string& webhook_id, const std::string& webhook_token, const Json& body,
                                                   const std::optional<std::string>& thread_id, bool wait) {
    const std::string path = "/webhooks/" + enc(webhook_id) + "/" + enc(webhook_token);
    RequestOptions o;
    o.auth = false;
    o.query.emplace_back("wait", wait ? "true" : "false");
    if (thread_id) o.query.emplace_back("thread_id", *thread_id);
    const Json res = request("POST", path, body, o);
    if (!wait || !res.is_object()) return std::nullopt;
    return convert<Message>(res, redact_route("POST", path), message_from_json);
}

void DiscordApi::create_interaction_response(const std::string& interaction_id, const std::string& interaction_token, const Json& body,
                                             const std::vector<FileUpload>& files) {
    RequestOptions o;
    o.auth = false;
    o.files = files;
    request("POST", "/interactions/" + enc(interaction_id) + "/" + enc(interaction_token) + "/callback", body, o);
}

Message DiscordApi::edit_original_interaction_response(const std::string& application_id, const std::string& interaction_token,
                                                       const Json& body, const std::vector<FileUpload>& files) {
    const std::string path = "/webhooks/" + enc(application_id) + "/" + enc(interaction_token) + "/messages/@original";
    RequestOptions o;
    o.auth = false;
    o.files = files;
    return convert<Message>(request("PATCH", path, body, o), redact_route("PATCH", path), message_from_json);
}

Message DiscordApi::create_followup_message(const std::string& application_id, const std::string& interaction_token, const Json& body,
                                            const std::vector<FileUpload>& files) {
    const std::string path = "/webhooks/" + enc(application_id) + "/" + enc(interaction_token);
    RequestOptions o;
    o.auth = false;
    o.files = files;
    o.query.emplace_back("wait", "true");
    return convert<Message>(request("POST", path, body, o), redact_route("POST", path), message_from_json);
}

GatewayBot DiscordApi::get_gateway_bot() { return convert<GatewayBot>(request("GET", "/gateway/bot"), "GET /gateway/bot", gateway_bot_from_json); }

Json DiscordApi::bulk_overwrite_commands(const std::string& application_id, const std::optional<std::string>& guild_id, const Json& commands) {
    const std::string path = guild_id ? "/applications/" + enc(application_id) + "/guilds/" + enc(*guild_id) + "/commands"
                                      : "/applications/" + enc(application_id) + "/commands";
    return request("PUT", path, commands);
}

std::uint64_t DiscordApi::current_member_permissions(const std::string& guild_id) {
    const User me = get_current_user();
    const Guild guild = get_guild(guild_id);
    if (guild.owner_id && *guild.owner_id == me.id) return permission::All;
    const Member member = get_guild_member(guild_id, me.id);
    return compute_base_permissions(guild, member, me.id);
}

} // namespace di::discord_hook
