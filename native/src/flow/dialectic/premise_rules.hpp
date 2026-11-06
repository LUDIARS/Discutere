#pragma once
// src/flow/dialectic/premise-rules.ts の移植: 議論の前提ルール (dialectic.md §2.5) — 型 + 正規化 + プロンプトへ載せるルール本文。
// LLM なし・判断なし。違反の検査は rule-check が持つ (SRP)。
#include <optional>
#include <string>
#include <string_view>

#include "base/json.hpp"

namespace di::flow::dialectic {

/** 論拠の種類: "deduction" | "induction" | "abduction"。 */
using InferenceKind = std::string;
/** 反論が突くトゥールミン要素: "data" | "warrant" | "qualifier"。 */
using AttackPoint = std::string;

/** 日本語ラベル (演繹 / 帰納 / アブダクション)。未知は空。 */
std::string inference_kind_label(const InferenceKind& kind);
/** 日本語ラベル (データ / 論拠 / 限定)。未知は空。 */
std::string attack_point_label(const AttackPoint& point);

/** LLM 由来の値を InferenceKind に正規化する (未知値・文字列以外は nullopt)。日本語ラベルも受ける。 */
std::optional<InferenceKind> coerce_inference_kind(const Json& v);
/** LLM 由来の値を AttackPoint に正規化する (未知値・文字列以外は nullopt)。日本語ラベルも受ける。 */
std::optional<AttackPoint> coerce_attack_point(const Json& v);

/** 全発話プロンプト (定立・反定立) に載せる前提ルール本文。 */
const std::string& premise_rules_text();

} // namespace di::flow::dialectic
