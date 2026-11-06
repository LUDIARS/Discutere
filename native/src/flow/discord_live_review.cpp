#include "flow/discord_live_internal.hpp"

namespace di::flow {
namespace dh = di::discord_hook;

namespace discord_live_detail {


user_voices::VoiceRequestPort voice_request_of(const FlowDiscordDeps& deps) {
    return deps.voiceRequest ? *deps.voiceRequest : user_voices::configured_voice_request();
}






void schedule_review_auto_start(const std::string& thread_id, const FlowDiscordDeps& deps) {
    const auto pending = find_review(thread_id);
    if (!pending) return;
    if (user_voices::needs_voice_choice(user_voices::read_voice_preparation(forum_session_id(thread_id)))) return;
    const double timeout_ms = config::get_config().flow.paperReview.timeoutMs;
    if (!(timeout_ms > 0)) return;
    std::lock_guard lock(g_mu);
    cancel_timer_locked(*pending);
    // タイマーのスレッドでは重い仕事をしない: 背景キューへ渡し、返信と同じ直列キューで自動開始する。
    pending->timer = global_scheduler().after(std::chrono::milliseconds(static_cast<long long>(timeout_ms)), [thread_id, deps] {
        const bool queued = submit_background(deps, [thread_id, deps] {
            try {
                run_serial(thread_id, [&] {
                    const auto p = find_review(thread_id);
                    if (!p) return;
                    approve_pending(thread_id, p, deps, p->hooks.value_or(FlowLiveHooks{}), "⏱️ 無操作のため草案のまま議論を始めます…");
                });
            } catch (const std::exception& e) {
                log::warn("  flow-live: ペーパー自動開始失敗 (thread=" + thread_id + "): " + error_message(e));
            }
        });
        if (!queued) log::warn("  flow-live: 背景キューが満杯のためペーパーの自動開始を見送る (thread=" + thread_id + ")");
    });
}

} // namespace discord_live_detail
} // namespace di::flow
