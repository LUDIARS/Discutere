#pragma once
// 議題文 → 検索キーワード抽出 (src/discatier-engine-adapter/keyword-terms.ts)。
// 形態素解析器は持たない。句読点・空白・代表的な助詞/接続で素朴に分割し、長さ 2 以上の語を拾う。
// TS の正規表現 SPLIT を線形の走査で写す。純関数。
#include <cstddef>
#include <string>
#include <vector>

namespace di::discatier_engine_adapter {

/** 議題語 (terms) を検索キーワードに分解する (長い語を優先して最大 max 語)。 */
std::vector<std::string> extract_key_terms(const std::vector<std::string>& terms, std::size_t max = 6);

} // namespace di::discatier_engine_adapter
