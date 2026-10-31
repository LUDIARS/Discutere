#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace di::env {

/** process.env[name] (未設定は nullopt)。 */
std::optional<std::string> get(std::string_view name);
/** 未設定または空文字 (trim 後) なら nullopt。 */
std::optional<std::string> get_nonempty(std::string_view name);
std::string get_or(std::string_view name, std::string_view fallback);
/** 現在の環境変数の写し (子プロセスに渡す env を作る元)。 */
std::map<std::string, std::string> snapshot();
/** テスト用: プロセスの環境変数を書き換える (本番コードでは使わない)。 */
void set_for_test(std::string_view name, std::optional<std::string_view> value);
/** 現在の作業ディレクトリ (UTF-8)。 */
std::string cwd();
/** 現在のプロセス ID。 */
std::int64_t pid();

} // namespace di::env
