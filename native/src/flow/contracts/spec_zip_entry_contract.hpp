#pragma once
// src/flow/contracts/spec-zip-entry.contract.ts の移植: 仕様書 ZIP の 1 エントリ展開 (inflateZipEntry) の契約。
// ZIP の展開そのものは Node のサイドカー (native/sidecar/spec-extract.ts) が行い、そこでは TS の契約がそのまま効く。
// C++ 側では同じ判定を関数として持つ (サイドカーの結果の検査・テストに使う)。
#include <cstddef>
#include <string>
#include <string_view>

#include "flow/contracts/runtime.hpp"

namespace di::flow::contracts {

/** post: 展開結果が 1 エントリ 64MiB 以内かつ残りの総量 (maxBytes) 以内。 */
ContractVerdict spec_zip_entry_post(std::size_t result_bytes, std::size_t max_bytes);
/** postThrow: 失敗の文言が "<label>: " で始まり、どの仕様書かを示している。 */
ContractVerdict spec_zip_entry_post_throw(std::string_view error_message, std::string_view label);

} // namespace di::flow::contracts
