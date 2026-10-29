#pragma once
// @ludiars/llm-gateway の共通型 (lib/lapilli/packages/llm-gateway/src/types.ts)。
// このライブラリは transport 非依存 ── HTTP も子プロセスも呼ばない。
// 「モデルへ送る前のプロンプトをどう整形するか」 だけを扱う純ロジック。
#include <string>

#include "base/json.hpp"

namespace di::lib::llm_gateway {

/** チャットメッセージの役割 ('system' | 'user' | 'assistant')。OpenAI / Anthropic 共通の最小集合。 */
using ChatRole = std::string;

/** モデルへ渡す 1 メッセージ。 */
struct ChatMessage {
    ChatRole role;
    std::string content;
    bool operator==(const ChatMessage&) const = default;
};

/**
 * プロンプト断片の安定度 ('fixed' | 'semi' | 'volatile')。prefix-cache のヒット率はこの順序付けで決まる。
 * - fixed    : 呼び出しを跨いで不変 (人格・規約・ツール定義)。キャッシュの錨。
 * - semi     : セッション内では不変 (添付資料・RAG 結果)。
 * - volatile : 毎回変わる (会話履歴末尾・日時・今回の入力)。
 */
using Stability = std::string;

/** 安定度タグ付きのプロンプト断片。 */
struct Segment {
    Stability stability;
    ChatRole role;
    std::string text;
};

inline Json to_json(const ChatMessage& m) { return Json{{"role", m.role}, {"content", m.content}}; }

} // namespace di::lib::llm_gateway
