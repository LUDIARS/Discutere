#pragma once
// 外部の声検索 (listExternalVoices) の配線ヘルパ (src/flow/external-voices.ts)。
//
// 議論フロー (web / discord) の deps が要求する `(terms, limit) => ContextVoice[]` を、
// Discatier Core の adapter (list_relevant_external_voices) から組み立てる。
// Core はコール毎に open/close する。議論本体は director が voice-cache で memoize するため呼び出し頻度は低い。
//
// 露出は出所 (source + URL) のみ透過、個人アンカーは adapter 側で仮名にマスク済み (§6)。
// `機密` / `内部` タグでは呼び出し側が gate_external_voices (flow/tags) でこの lookup ごと外す。
#include <functional>
#include <memory>
#include <string>

#include "core/index.hpp"
#include "flow/voice_cache.hpp"

namespace di::flow {

using OpenCoreFn = std::function<std::unique_ptr<core::Core>()>;

/**
 * active KG から外部の声を引く listExternalVoices を作る。
 * @param open_core active KG を開く factory (resolve_active_kg_path 経由)。
 * @param workspace_id 匿名 workspace (個人データ非保管)。
 * 検索中の例外は空配列にする (議論を止めない)。open_core の例外は TS と同じく呼び出し側へ投げる。
 */
ListExternalVoicesFn make_list_external_voices(OpenCoreFn open_core, std::string workspace_id);

} // namespace di::flow
