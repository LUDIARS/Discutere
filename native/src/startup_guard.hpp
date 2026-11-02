#pragma once
// production 起動時の health guard (src/startup-guard.ts)。
//
// NODE_ENV=production のとき起動時に最低限の健全性を fail-fast で検証し、運用ミス (token 空のまま本番起動等) を
// 黙って無効化しない (作法: 無言フォールバック禁止)。dev/test では no-op。
//   致命 (throw): discord.botToken が空。
//   警告 (起動は続行): discord.adminIds が空 (admin slash コマンドは全 deny = 安全 default) / llm.backend=mock。
// 秘密値 (bot token 等) はメッセージ・ログに出さない (設定の有無だけを書く)。
// dev default の秘密値: config には既定の秘密値が無い (未設定は空文字で、各経路が default-deny する) ので
// 拒否すべき既定値は無い。TS と同じ検査だけを行う。
#include <functional>
#include <stdexcept>
#include <string>

#include "config.hpp"

namespace di {

class StartupHealthError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

using StartupWarn = std::function<void(const std::string&)>;

/**
 * production の起動前提を検証する。致命は StartupHealthError、推奨外は warn (既定 log::warn)。
 */
void assert_startup_health(const config::DiscutereConfig& config, const StartupWarn& warn = {});

} // namespace di
