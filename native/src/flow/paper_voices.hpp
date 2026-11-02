#pragma once
// src/flow/paper-voices.ts の移植: ディスカッションペーパーの声の保存と検索 (spec/feature/flow/paper-reuse.md)。
// 草案作成時に集めた声をセッションごとに保存し、ペーパーの使い回し・再起動後のやり直しで集め直さずに使う。
// 保存するのは本文・出所・元 URL だけ (個人アンカーは持たない)。
#include <optional>
#include <string>
#include <vector>

#include "flow/discussion_paper.hpp"
#include "flow/voice_cache.hpp"

namespace di::flow {

/** 1 ペーパーに保存する声の上限。 */
inline constexpr std::size_t PAPER_VOICE_LIMIT = 200;

using VoiceLookup = ListExternalVoicesFn;

/** セッションの声を保存する (置き換え。空なら何もしない = 既存を消さない)。保存件数を返す。 */
std::size_t save_paper_voices(const std::string& session_id, const std::vector<ContextVoice>& voices);
/** セッションに保存した声 (保存順)。 */
std::vector<ContextVoice> load_paper_voices(const std::string& session_id);
/** 保存した声から検索する (検索語を多く含む声を先に、同点は保存順。一致が無くても返す)。 */
VoiceLookup stored_voice_lookup(std::vector<ContextVoice> voices);
/** セッションに保存した声があればその検索、無ければ fallback (空の関数 = 無し)。 */
VoiceLookup session_voice_lookup(const std::string& session_id, VoiceLookup fallback = {});

} // namespace di::flow
