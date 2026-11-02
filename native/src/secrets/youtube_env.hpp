#pragma once
// YouTube API キーの読み出し (src/secrets/youtube-env.ts の移植)。
// Excubitor が起動時に Vault の秘密を env へ注入する。アプリは env を読むだけで、キーの更新 API は持たない。
#include <map>
#include <optional>
#include <string>

#include "base/json.hpp"

namespace di::secrets {

using EnvMap = std::map<std::string, std::string>;

/** env の DISCUTERE_YOUTUBE_API_KEY (trim 後に空なら nullopt)。引数なしはプロセスの環境変数。 */
std::optional<std::string> get_youtube_api_key();
std::optional<std::string> get_youtube_api_key(const EnvMap& env);

/** 有無だけを返す (秘密そのものは出さない)。JSON は {"present": bool}。 */
struct YoutubeApiKeyStatus {
    bool present = false;
};
YoutubeApiKeyStatus get_youtube_api_key_status();
YoutubeApiKeyStatus get_youtube_api_key_status(const EnvMap& env);
Json to_json(const YoutubeApiKeyStatus& status);

} // namespace di::secrets
