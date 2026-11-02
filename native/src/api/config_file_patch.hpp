#pragma once
// admin 画面から config ファイル (discutere.config.json) の一部を書き戻す補助 (w3-O)。
// persona-routes (flow.autoAdoptOnCrawl) と datasource-routes (discatier.activeKg) が使う。
// TS は JSON.parse(readFileSync) → 変更 → writeFileSync(JSON.stringify(x, null, 2) + "\n")。
// C++ は読み込みに上限 (config::kMaxConfigFileBytes) を課し、書き込みは一時ファイル経由の置き換え。
// 同じプロセス内の書き込みは mutex で直列化する。
#include <functional>
#include <string>

#include "base/json.hpp"

namespace di::api {

/** path.resolve(process.env.DISCUTERE_CONFIG ?? "./discutere.config.json") */
std::string config_file_path();

/**
 * config ファイルを読み (無ければ {})、mutate で書き換えて書き戻す。
 * 読めない・JSON でない・object でない・書けないときは std::runtime_error (メッセージに秘密値は含めない)。
 */
void patch_config_file(const std::function<void(Json& current)>& mutate);

} // namespace di::api
