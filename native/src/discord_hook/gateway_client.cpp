#include "discord_hook/gateway_client.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>

#include "base/crypto.hpp"
#include "base/log.hpp"
#include "base/url.hpp"
#include "infra/ws_client.hpp"

namespace di::discord_hook {

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

// ─── 純関数・小さな部品 ─────────────────────────────────────────

GatewayCloseAction classify_close_code(int code) {
    // 4004 認証失敗 / 4010 shard 不正 / 4011 sharding 必須 / 4012 API 版不正 / 4013 intents 不正 / 4014 許可されていない intents
    if (code == 4004 || (code >= 4010 && code <= 4014)) return GatewayCloseAction::Fatal;
    // 4003 未認証で送った / 4007 seq 不正 / 4009 session 期限切れ / 1000 正常 close (Discord は session を無効にする)
    if (code == 1000 || code == 4003 || code == 4007 || code == 4009) return GatewayCloseAction::Reidentify;
    return GatewayCloseAction::Resume;
}

std::string build_gateway_url(std::string_view base) {
    std::string s(base);
    const auto cut = s.find_first_of("?#");
    if (cut != std::string::npos) s.resize(cut);
    while (!s.empty() && s.back() == '/') s.pop_back();
    return s + "/?v=10&encoding=json";
}

namespace {
bool host_is_or_under(const std::string& host, std::string_view domain) {
    if (host == domain) return true;
    return host.size() > domain.size() + 1 && host.compare(host.size() - domain.size(), domain.size(), domain) == 0 &&
           host[host.size() - domain.size() - 1] == '.';
}
} // namespace

bool is_allowed_gateway_url(std::string_view text, bool allow_insecure_test_endpoints) {
    auto u = url::parse(text);
    if (!u || u->host.empty()) return false;
    if (allow_insecure_test_endpoints) return u->scheme == "ws" || u->scheme == "wss";
    if (u->scheme != "wss") return false;
    std::string host = u->host;
    for (auto& c : host) c = static_cast<char>((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
    return host_is_or_under(host, "discord.gg") || host_is_or_under(host, "discord.com");
}

GatewaySendLimiter::GatewaySendLimiter(std::size_t limit, std::size_t reserved, milliseconds window)
    : limit_(std::max<std::size_t>(limit, 1)), reserved_(std::min(reserved, std::max<std::size_t>(limit, 1) - 1)), window_(window) {}

void GatewaySendLimiter::prune(Clock::time_point now) {
    while (!sent_.empty() && sent_.front() + window_ <= now) sent_.pop_front();
}

bool GatewaySendLimiter::try_acquire(bool priority, Clock::time_point now) {
    prune(now);
    const std::size_t cap = priority ? limit_ : limit_ - reserved_;
    if (sent_.size() >= cap) return false;
    sent_.push_back(now);
    return true;
}

Clock::time_point GatewaySendLimiter::next_free(bool priority, Clock::time_point now) {
    prune(now);
    const std::size_t cap = priority ? limit_ : limit_ - reserved_;
    if (sent_.size() < cap) return now;
    // cap 件を下回るには、古い方から (size - cap + 1) 件目が窓から出る必要がある。
    return sent_[sent_.size() - cap] + window_;
}

std::size_t GatewaySendLimiter::used(Clock::time_point now) {
    prune(now);
    return sent_.size();
}

void GatewaySendLimiter::reset() { sent_.clear(); }

GatewayDedupe::GatewayDedupe(std::size_t capacity, milliseconds ttl) : capacity_(std::max<std::size_t>(capacity, 1)), ttl_(ttl) {}

void GatewayDedupe::prune(Clock::time_point now) {
    while (!order_.empty() && (order_.front().first + ttl_ <= now || order_.size() > capacity_)) {
        auto it = seen_.find(order_.front().second);
        // forget 後に同じ鍵を覚え直した場合は、古い順序の記録だけを捨てる。
        if (it != seen_.end() && it->second == order_.front().first) seen_.erase(it);
        order_.pop_front();
    }
}

bool GatewayDedupe::first_seen(const std::string& key, Clock::time_point now) {
    prune(now);
    if (seen_.count(key)) return false;
    seen_[key] = now;
    order_.emplace_back(now, key);
    prune(now);
    return true;
}

void GatewayDedupe::forget(const std::string& key) { seen_.erase(key); }

std::optional<std::string> gateway_dedupe_key(std::string_view event, const Json& d) {
    if (!d.is_object()) return std::nullopt;
    if (event == "MESSAGE_CREATE" || event == "INTERACTION_CREATE" || event == "THREAD_CREATE") {
        auto id = json_opt_string(d, "id");
        if (!id || id->empty()) return std::nullopt;
        return std::string(event) + ":" + *id;
    }
    if (event == "MESSAGE_REACTION_ADD" || event == "MESSAGE_REACTION_REMOVE") {
        auto message_id = json_opt_string(d, "message_id");
        auto user_id = json_opt_string(d, "user_id");
        if (!message_id || !user_id) return std::nullopt;
        const Json& emoji = json_object(d, "emoji");
        std::string e = json_string(emoji, "id");
        if (e.empty()) e = json_string(emoji, "name");
        // REMOVE は ADD の鍵を忘れるために同じ鍵を返す。
        return "MESSAGE_REACTION_ADD:" + *message_id + ":" + *user_id + ":" + e;
    }
    return std::nullopt;
}

// ─── クライアント本体 ───────────────────────────────────────────

struct GatewayClient::Impl {
    explicit Impl(GatewayClientOptions options)
        : o(std::move(options)),
          limiter(o.send_limit, o.send_reserved, o.send_window),
          dedupe(o.dedupe_capacity, o.dedupe_ttl) {
        DiscordApiOptions rest = o.rest;
        if (rest.token.empty()) rest.token = o.token;
        api = std::make_unique<DiscordApi>(std::move(rest));
    }

    GatewayClientOptions o;
    std::unique_ptr<DiscordApi> api;

    // ── m で守る (他スレッドと共有) ──
    mutable std::mutex m;
    std::condition_variable cv;
    bool stop_requested = false;
    bool started = false;
    struct Inbound {
        std::uint64_t gen = 0;
        bool closed = false;
        std::string text;
        int code = 0;
        std::string reason;
    };
    std::deque<Inbound> inbox;
    std::deque<std::string> outbox;
    bool outbox_dirty = false;
    bool is_ready = false;
    GatewayStats st;
    std::thread thread;
    std::atomic<bool> is_running{false};

    // ── 制御スレッドだけが触る ──
    std::optional<std::string> session_id;
    std::optional<std::string> resume_url;
    std::int64_t seq = -1;
    std::shared_ptr<ws::Client> ws;
    std::uint64_t gen = 0;
    GatewaySendLimiter limiter;
    GatewayDedupe dedupe;
    std::deque<Clock::time_point> identify_times;
    Clock::time_point last_heartbeat_sent{};

    enum class Kind { Reconnect, Fatal, Stopped };
    struct Outcome {
        Kind kind = Kind::Reconnect;
        /** true なら失敗として数えない (接続が確立していた・サーバの指示による再接続)。 */
        bool ok = false;
        std::optional<milliseconds> delay;
        int code = 0;
        std::string reason;
    };
    static Outcome reconnect(bool ok, std::optional<milliseconds> delay = std::nullopt) {
        Outcome r;
        r.ok = ok;
        r.delay = delay;
        return r;
    }
    static Outcome stopped() {
        Outcome r;
        r.kind = Kind::Stopped;
        return r;
    }
    static Outcome fatal(int code, std::string reason) {
        Outcome r;
        r.kind = Kind::Fatal;
        r.code = code;
        r.reason = std::move(reason);
        return r;
    }

    bool stopping() const {
        std::lock_guard lock(m);
        return stop_requested;
    }

    /** 停止要求で起きたら true。 */
    bool wait_stop(milliseconds d) {
        std::unique_lock lock(m);
        return cv.wait_for(lock, d, [&] { return stop_requested; });
    }

    void set_ready(bool v) {
        std::lock_guard lock(m);
        is_ready = v;
        if (!v) outbox.clear();
    }

    void clear_session() {
        session_id.reset();
        resume_url.reset();
        seq = -1;
        std::lock_guard lock(m);
        st.has_session = false;
        st.seq = -1;
    }

    milliseconds backoff(int failures) const {
        const long long base = std::max<long long>(o.reconnect_base.count(), 1);
        const long long cap = std::max<long long>(o.reconnect_max.count(), base);
        const long long exp = std::min<long long>(base << std::min(failures, 20), cap);
        // 指数 backoff の半分 + 一様な jitter (半分)。
        return milliseconds(exp / 2 + static_cast<long long>(crypto::random_unit() * static_cast<double>(exp - exp / 2)));
    }

    void loop() {
        int failures = 0;
        std::optional<milliseconds> delay;
        while (!stopping()) {
            if (delay && delay->count() > 0 && wait_stop(*delay)) break;
            Outcome r = run_connection();
            close_socket();
            set_ready(false);
            if (r.kind == Kind::Stopped) break;
            if (r.kind == Kind::Fatal) {
                log::error("[gateway] stopped: close " + std::to_string(r.code) + " " + r.reason + " (再接続しない)");
                if (o.on_fatal) {
                    try {
                        o.on_fatal(r.code, r.reason);
                    } catch (...) {
                    }
                }
                break;
            }
            failures = r.ok ? 0 : failures + 1;
            if (o.max_consecutive_failures > 0 && failures >= o.max_consecutive_failures) {
                log::error("[gateway] 接続に " + std::to_string(failures) + " 回続けて失敗したので諦める");
                if (o.on_fatal) {
                    try {
                        o.on_fatal(0, "too many consecutive failures");
                    } catch (...) {
                    }
                }
                break;
            }
            delay = r.delay ? *r.delay : backoff(failures);
        }
        close_socket();
        set_ready(false);
        is_running = false;
    }

    void close_socket() {
        // ws::Client のデストラクタが I/O スレッドを止めて待つ。制御スレッド (I/O スレッドではない) で壊す。
        ws.reset();
        std::lock_guard lock(m);
        inbox.clear();
    }

    bool send_raw(const Json& payload, bool priority) {
        const auto now = Clock::now();
        if (!limiter.try_acquire(priority, now)) {
            // heartbeat / identify / resume は reserved の内側で足りる設計。足りないなら送って warn (送らないとゾンビ扱いになる)。
            log::warn("[gateway] send rate limit exhausted for op " + std::to_string(json_int(payload, "op")));
        }
        return ws && ws->send(json_stringify(payload));
    }

    void send_heartbeat() {
        Json p = Json::object();
        p["op"] = gateway_op::Heartbeat;
        p["d"] = seq >= 0 ? Json(seq) : Json(nullptr);
        last_heartbeat_sent = Clock::now();
        send_raw(p, true);
        std::lock_guard lock(m);
        ++st.heartbeats_sent;
    }

    void send_identify() {
        Json props = Json::object();
        props["os"] = o.os;
        props["browser"] = o.browser;
        props["device"] = o.device;
        Json d = Json::object();
        d["token"] = o.token;
        d["intents"] = o.intents;
        d["properties"] = props;
        Json p = Json::object();
        p["op"] = gateway_op::Identify;
        p["d"] = d;
        identify_times.push_back(Clock::now());
        send_raw(p, true);
        std::lock_guard lock(m);
        ++st.identifies;
    }

    void send_resume() {
        Json d = Json::object();
        d["token"] = o.token;
        d["session_id"] = *session_id;
        d["seq"] = seq >= 0 ? Json(seq) : Json(nullptr);
        Json p = Json::object();
        p["op"] = gateway_op::Resume;
        p["d"] = d;
        send_raw(p, true);
        std::lock_guard lock(m);
        ++st.resumes;
    }

    void dispatch(const std::string& t, Json d) {
        {
            std::lock_guard lock(m);
            ++st.dispatched;
        }
        if (!o.on_dispatch) return;
        if (!o.dispatch_in_background) {
            try {
                o.on_dispatch(t, d);
            } catch (const std::exception& e) {
                log::error("[gateway] handler failed for " + t + ": " + e.what());
            }
            return;
        }
        Executor& ex = o.executor ? *o.executor : background_executor();
        auto fn = o.on_dispatch;
        const bool ok = ex.submit([fn, t, d = std::move(d)] {
            try {
                fn(t, d);
            } catch (const std::exception& e) {
                log::error("[gateway] handler failed for " + t + ": " + e.what());
            }
        });
        if (!ok) {
            std::lock_guard lock(m);
            ++st.dropped_queue_full;
            log::warn("[gateway] executor queue full: dropped " + t + " (dropped total " + std::to_string(st.dropped_queue_full) + ")");
        }
    }

    /** 新しい session を始める前の準備 (/gateway/bot・session_start_limit・max_concurrency)。URL を返す。 */
    std::optional<std::string> prepare_identify(Outcome& out) {
        GatewayBot bot;
        try {
            bot = api->get_gateway_bot();
        } catch (const DiscordApiError& e) {
            if (e.status() == 401) {
                out = fatal(401, "GET /gateway/bot unauthorized");
            } else {
                log::warn(std::string("[gateway] GET /gateway/bot failed: ") + e.what());
                out = reconnect(false);
            }
            return std::nullopt;
        } catch (const std::exception& e) {
            log::warn(std::string("[gateway] GET /gateway/bot failed: ") + e.what());
            out = reconnect(false);
            return std::nullopt;
        }
        if (bot.session_start_total > 0 && bot.session_start_remaining <= 0) {
            const auto wait = std::min(milliseconds(std::max<std::int64_t>(bot.session_start_reset_after, 0)), o.max_session_start_wait);
            log::warn("[gateway] session_start_limit exhausted; waiting " + std::to_string(wait.count()) + "ms before identify");
            if (wait_stop(wait)) {
                out = stopped();
                return std::nullopt;
            }
            out = reconnect(true, milliseconds(0));  // 取り直してから identify する
            return std::nullopt;
        }
        // max_concurrency: identify_window あたり max_concurrency 回まで。
        const std::size_t mc = static_cast<std::size_t>(std::max(bot.max_concurrency, 1));
        while (true) {
            const auto now = Clock::now();
            while (!identify_times.empty() && identify_times.front() + o.identify_window <= now) identify_times.pop_front();
            if (identify_times.size() < mc) break;
            const auto until = identify_times[identify_times.size() - mc] + o.identify_window;
            if (wait_stop(std::chrono::duration_cast<milliseconds>(until - now) + milliseconds(1))) {
                out = stopped();
                return std::nullopt;
            }
        }
        return bot.url;
    }

    Outcome run_connection() {
        const bool resuming = session_id.has_value() && resume_url.has_value();
        std::string base;
        if (resuming) {
            base = *resume_url;
        } else {
            clear_session();
            Outcome early;
            auto u = prepare_identify(early);
            if (!u) return early;
            base = *u;
        }
        const std::string target = build_gateway_url(base);
        if (!is_allowed_gateway_url(target, o.allow_insecure_test_endpoints)) {
            log::warn("[gateway] refused gateway url (scheme/host not allowed)");
            if (resuming) clear_session();
            return reconnect(false);
        }

        ws::Options wo;
        wo.url = target;
        wo.connect_timeout = o.connect_timeout;
        wo.max_message_bytes = o.max_message_bytes;
        wo.send_queue_limit = 256;
        wo.allow_private_network = o.allow_insecure_test_endpoints;
        const std::uint64_t my_gen = ++gen;
        std::string error;
        {
            std::lock_guard lock(m);
            inbox.clear();
        }
        ws = ws::Client::connect(
            wo,
            [this, my_gen](std::string message) {
                std::lock_guard lock(m);
                inbox.push_back(Inbound{my_gen, false, std::move(message), 0, {}});
                cv.notify_all();
            },
            [this, my_gen](int code, std::string reason) {
                std::lock_guard lock(m);
                inbox.push_back(Inbound{my_gen, true, {}, code, std::move(reason)});
                cv.notify_all();
            },
            &error);
        if (!ws) {
            log::warn("[gateway] connect failed: " + error);
            return reconnect(false);
        }
        {
            std::lock_guard lock(m);
            ++st.connects;
        }
        return session(resuming, my_gen);
    }

    Outcome session(bool resuming, std::uint64_t my_gen) {
        const auto hello_deadline = Clock::now() + o.hello_timeout;
        bool hello = false;
        bool ack_ok = true;
        bool established = false;
        milliseconds interval{0};
        Clock::time_point next_hb{};

        while (true) {
            Clock::time_point deadline = hello ? next_hb : hello_deadline;
            bool want_drain = false;
            {
                std::lock_guard lock(m);
                want_drain = is_ready && !outbox.empty();
            }
            if (want_drain) deadline = std::min(deadline, limiter.next_free(false, Clock::now()));

            std::deque<Inbound> batch;
            bool stop_now = false;
            {
                std::unique_lock lock(m);
                cv.wait_until(lock, deadline, [&] { return stop_requested || !inbox.empty() || outbox_dirty; });
                outbox_dirty = false;
                stop_now = stop_requested;
                batch.swap(inbox);
            }
            if (stop_now) return graceful_stop(my_gen);

            for (auto& in : batch) {
                if (in.gen != my_gen) continue;
                if (in.closed) {
                    const auto action = classify_close_code(in.code);
                    if (action == GatewayCloseAction::Fatal) return fatal(in.code, in.reason);
                    log::warn("[gateway] closed " + std::to_string(in.code) + " " + in.reason +
                              (action == GatewayCloseAction::Reidentify ? " → identify" : " → resume"));
                    if (action == GatewayCloseAction::Reidentify) clear_session();
                    return reconnect(established);
                }
                std::string err;
                auto parsed = parse_json(in.text, o.json_limits, &err);
                if (!parsed || !parsed->is_object()) {
                    std::lock_guard lock(m);
                    ++st.malformed_frames;
                    log::warn("[gateway] malformed frame dropped (" + std::to_string(in.text.size()) + " bytes)");
                    continue;
                }
                Json& j = *parsed;
                const int op = static_cast<int>(json_int(j, "op", -1));
                switch (op) {
                case gateway_op::Hello: {
                    if (hello) break;
                    const Json& d = json_object(j, "d");
                    const auto hi = json_opt_number(d, "heartbeat_interval");
                    if (!hi || *hi <= 0) {
                        log::warn("[gateway] Hello without heartbeat_interval");
                        return reconnect(false);
                    }
                    interval = std::clamp(milliseconds(static_cast<long long>(*hi)), o.min_heartbeat_interval, o.max_heartbeat_interval);
                    hello = true;
                    ack_ok = true;
                    // 初回は interval × jitter (0..1) で送る (Discord の推奨)。
                    next_hb = Clock::now() + milliseconds(static_cast<long long>(static_cast<double>(interval.count()) * crypto::random_unit()));
                    if (resuming) send_resume();
                    else send_identify();
                    break;
                }
                case gateway_op::HeartbeatAck: {
                    ack_ok = true;
                    std::lock_guard lock(m);
                    ++st.heartbeat_acks;
                    st.last_latency_ms = std::chrono::duration_cast<milliseconds>(Clock::now() - last_heartbeat_sent).count();
                    break;
                }
                case gateway_op::Heartbeat:
                    // サーバからの要求には即座に応答する (定期の予定は変えない)。
                    send_heartbeat();
                    break;
                case gateway_op::Reconnect:
                    log::info("[gateway] op 7 Reconnect → resume");
                    return reconnect(true);
                case gateway_op::InvalidSession: {
                    const bool resumable = j.contains("d") && j["d"].is_boolean() && j["d"].get<bool>();
                    if (resumable && session_id) {
                        log::warn("[gateway] op 9 Invalid Session (resumable) → resume");
                        return reconnect(true);
                    }
                    log::warn("[gateway] op 9 Invalid Session → identify");
                    clear_session();
                    const auto lo = o.invalid_session_wait_min.count();
                    const auto hi = std::max(o.invalid_session_wait_max.count(), lo);
                    const auto wait = milliseconds(lo + static_cast<long long>(crypto::random_unit() * static_cast<double>(hi - lo)));
                    return reconnect(true, wait);
                }
                case gateway_op::Dispatch:
                    if (handle_dispatch(j)) established = true;
                    break;
                default:
                    break;
                }
            }

            const auto now = Clock::now();
            if (!hello && now >= hello_deadline) {
                log::warn("[gateway] no Hello within " + std::to_string(o.hello_timeout.count()) + "ms → reconnect");
                return reconnect(false);
            }
            if (hello && now >= next_hb) {
                if (!ack_ok) {
                    std::lock_guard lock(m);
                    ++st.zombie_reconnects;
                    log::warn("[gateway] heartbeat ACK missing (zombie connection) → resume");
                    return reconnect(established);
                }
                send_heartbeat();
                ack_ok = false;
                next_hb = now + interval;
            }
            drain_outbox();
        }
    }

    /** READY / RESUMED を受けたら true。 */
    bool handle_dispatch(Json& j) {
        if (!j.contains("t") || !j["t"].is_string()) return false;
        const std::string t = j["t"].get<std::string>();
        if (j.contains("s") && j["s"].is_number_integer()) {
            const std::int64_t s = j["s"].get<std::int64_t>();
            if (seq >= 0 && s <= seq) {
                std::lock_guard lock(m);
                ++st.stale_seq_dropped;
                return false;
            }
            seq = s;
            std::lock_guard lock(m);
            st.seq = s;
        }
        Json d = j.contains("d") ? std::move(j["d"]) : Json(nullptr);
        bool established = false;
        if (t == "READY") {
            auto sid = json_opt_string(d, "session_id");
            auto rurl = json_opt_string(d, "resume_gateway_url");
            if (sid && !sid->empty()) session_id = *sid;
            if (rurl && is_allowed_gateway_url(build_gateway_url(*rurl), o.allow_insecure_test_endpoints)) resume_url = *rurl;
            else resume_url.reset();  // 受け付けない URL なら resume せず identify し直す
            established = true;
            {
                std::lock_guard lock(m);
                st.has_session = session_id.has_value();
            }
            set_ready(true);
        } else if (t == "RESUMED") {
            established = true;
            set_ready(true);
        }
        if (auto key = gateway_dedupe_key(t, d)) {
            if (t == "MESSAGE_REACTION_REMOVE") {
                dedupe.forget(*key);
            } else if (!dedupe.first_seen(*key, Clock::now())) {
                std::lock_guard lock(m);
                ++st.duplicates_dropped;
                return established;
            }
        }
        dispatch(t, std::move(d));
        return established;
    }

    void drain_outbox() {
        while (true) {
            std::string text;
            {
                std::lock_guard lock(m);
                if (!is_ready || outbox.empty()) return;
                if (!limiter.try_acquire(false, Clock::now())) return;
                text = std::move(outbox.front());
                outbox.pop_front();
            }
            if (ws) ws->send(std::move(text));
        }
    }

    Outcome graceful_stop(std::uint64_t my_gen) {
        if (ws) {
            ws->close(1000, "");
            std::unique_lock lock(m);
            cv.wait_for(lock, o.close_grace, [&] {
                for (const auto& in : inbox)
                    if (in.gen == my_gen && in.closed) return true;
                return false;
            });
        }
        return stopped();
    }
};

GatewayClient::GatewayClient(GatewayClientOptions options) : impl_(std::make_shared<Impl>(std::move(options))) {}

GatewayClient::~GatewayClient() { stop(); }

void GatewayClient::start() {
    std::lock_guard lock(impl_->m);
    if (impl_->started || impl_->stop_requested) return;
    impl_->started = true;
    impl_->is_running = true;
    impl_->thread = std::thread([impl = impl_.get()] {
        try {
            impl->loop();
        } catch (const std::exception& e) {
            log::error(std::string("[gateway] control loop failed: ") + e.what());
            impl->close_socket();
            impl->is_running = false;
        }
    });
}

void GatewayClient::stop() {
    {
        std::lock_guard lock(impl_->m);
        impl_->stop_requested = true;
        impl_->cv.notify_all();
    }
    impl_->api->shutdown();
    if (impl_->thread.joinable()) {
        if (impl_->thread.get_id() == std::this_thread::get_id()) impl_->thread.detach();
        else impl_->thread.join();
    }
    impl_->is_running = false;
}

bool GatewayClient::running() const { return impl_->is_running; }

bool GatewayClient::ready() const {
    std::lock_guard lock(impl_->m);
    return impl_->is_ready;
}

bool GatewayClient::send(int op, const Json& d) {
    if (op != gateway_op::PresenceUpdate && op != gateway_op::VoiceStateUpdate && op != gateway_op::RequestGuildMembers) return false;
    Json p = Json::object();
    p["op"] = op;
    p["d"] = d;
    std::string text = json_stringify(p);
    if (text.size() > impl_->o.max_send_bytes) return false;
    std::lock_guard lock(impl_->m);
    if (!impl_->is_ready || impl_->stop_requested || impl_->outbox.size() >= impl_->o.max_send_queue) return false;
    impl_->outbox.push_back(std::move(text));
    impl_->outbox_dirty = true;
    impl_->cv.notify_all();
    return true;
}

GatewayStats GatewayClient::stats() const {
    std::lock_guard lock(impl_->m);
    return impl_->st;
}

} // namespace di::discord_hook
