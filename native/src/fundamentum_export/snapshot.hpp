#pragma once
// src/fundamentum-export/snapshot.ts の移植: 議論 (ConclusionDetail) を Fundamentum master に put できる
// 正規化 JSON スナップショットへ変換する (純関数・I/O なし)。
// 「いつ export したか」は含めない。master は content-addressed なので、議論が変わっていなければ
// 再 export しても id / catalog version が変わらない (dedup) のが正しい挙動。
#include "base/json.hpp"
#include "visualize/conclusions.hpp"

namespace di::fundamentum_export {

/** JSON.parse(JSON.stringify(raw)) と同じ値 (undefined のフィールドは落ちる)。 */
Json build_discussion_snapshot(const visualize::ConclusionDetail& detail);

} // namespace di::fundamentum_export
