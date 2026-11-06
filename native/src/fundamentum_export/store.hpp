#pragma once
// Fundamentum (lib/fundamentum) の Discutere 側シングルトン配線 (src/fundamentum-export/store.ts)。
// SRP: on-disk Foundation の遅延初期化のみ (dataDir は config.fundamentum.dataDir)。
#include <memory>

#include "lib/fundamentum/fundamentum.hpp"

namespace di::fundamentum_export {

/** 初回呼び出しで on-disk Foundation を開く (以後同じインスタンス。スレッド安全)。 */
lib::fundamentum::Foundation& get_fundamentum_store();

/** テスト用: シングルトンを差し替え可能にする (in-memory Foundation 等。nullptr で次回開き直す)。 */
void set_fundamentum_store_for_tests(std::shared_ptr<lib::fundamentum::Foundation> store);

} // namespace di::fundamentum_export
