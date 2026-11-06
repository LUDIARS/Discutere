#pragma once
// src/flow/paper-review-intent.ts の移植: ペーパーレビュー中のフォーラム返信を、キーワードで 3 種類の指示に振り分ける。
//   - adjust        : 議論内容の調整 (既定)
//   - voices        : 外部の声の取り込み
//   - similar_games : 類似するゲーム (メカニクスの代替)
// 承認 (「開始」) / 戻す の判定は呼び出し側で先に済ませる前提。純関数・副作用なし。
// 元の正規表現は、選択肢を JS の試行順に並べた語の列と線形の走査で置き換えた (std::regex は使わない)。
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "base/json.hpp"

namespace di::flow {

/** "adjust" | "voices" | "similar_games" */
using PaperReviewIntentKind = std::string;

struct PaperReviewIntent {
    PaperReviewIntentKind kind;
    /** 元の返信本文 (trim 済み)。 */
    std::string text;
    /** 指示から取り出した対象語 (voices = 検索語、similar_games = ゲーム名、adjust は空)。 */
    std::vector<std::string> terms;
};

/** 種類ごとの表示名 (スレッド通知に使う)。未知は空。 */
std::string paper_review_intent_label(const PaperReviewIntentKind& kind);

/** 指示になっていない返信か (相づち・否定・1 文字以下・記号や絵文字だけ)。 */
bool is_non_instruction_reply(std::string_view raw);

/** 返信本文を 3 種類の指示に振り分ける。両方のキーワードがあれば外部の声を優先する。 */
PaperReviewIntent classify_paper_review_intent(std::string_view raw);

/** JS の /[\p{L}\p{N}]/u に当たる文字か (Node から生成した範囲表)。 */
bool is_letter_or_number(std::uint32_t cp);

Json to_json(const PaperReviewIntent& intent);

} // namespace di::flow
