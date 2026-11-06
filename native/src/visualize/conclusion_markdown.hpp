#pragma once
// src/visualize/conclusion-markdown.ts の移植: 議論の結論 (ConclusionDetail) → 単体 markdown (議論 md エクスポート)。
// 旧フロー (design_gap) / 新フロー (flow_conclusion) どちらでも同じ形なので 1 つの純関数で md 化する。
// frontmatter (YAML、gray-matter の stringify と同じ書式) + 本文。個人アンカーは serializer 段でマスク済。
#include <string>

#include "visualize/conclusions.hpp"

namespace di::visualize {

/** ファイル名に使えない文字を除いた安全なスラグを作る (60 UTF-16 単位まで。空なら fallback)。 */
std::string safe_slug(const std::string& title, const std::string& fallback);

/**
 * 結論詳細を 1 本の markdown 文字列にする。結論まとめ → ペーパー → 止揚ストック → 高評価意見 → 議論ログ の順。
 * updatedAt が Date の範囲外 (toISOString が RangeError) なら std::range_error。
 */
std::string render_conclusion_markdown(const ConclusionDetail& detail);

} // namespace di::visualize
