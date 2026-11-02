#include "startup_guard.hpp"

#include <vector>

#include "base/log.hpp"
#include "base/strings.hpp"

namespace di {

void assert_startup_health(const config::DiscutereConfig& config, const StartupWarn& warn_fn) {
    if (config.nodeEnv != "production") return;
    const auto warn = [&](const std::string& m) {
        if (warn_fn) warn_fn(m);
        else log::warn(m);
    };

    // ── 致命: 欠けると議論 bot が機能しない ──
    std::vector<std::string> fatal;
    if (!config.discord.botToken || config.discord.botToken->empty()) {
        fatal.push_back("discord.botToken (DISCUTERE_DISCORD_BOT_TOKEN) が未設定。"
                        "production では Discord Gateway を起動できず議論 bot が機能しない");
    }
    if (!fatal.empty()) throw StartupHealthError("production 起動の前提条件を満たしていません:\n  - " + str::join(fatal, "\n  - "));

    // ── 致命ではないが運用上の注意 (起動は続行) ──
    if (config.discord.adminIds.empty()) {
        warn("[startup-guard] discord.adminIds が空です。admin slash コマンドは全 deny になります "
             "(DISCUTERE_DISCORD_ADMIN_IDS)");
    }
    if (config.llm.backend == "mock") warn("[startup-guard] production で llm.backend=mock です。実 LLM 応答は返りません");
}

} // namespace di
