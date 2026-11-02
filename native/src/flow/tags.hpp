#pragma once
// src/flow/tags.ts の移植: フロー共通タグ体系 (OVERVIEW §3)。
// 機密度 + 観点タグ。外部データ収集の可否とディスカッションペーパーの補足内容を決める。
// Discord フォーラムタグとは別軸。
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace di::flow {

/** "機密" | "内部" | "運用" | "開発" | "未来予測" | "代理" | 任意の文字列。 */
using FlowTag = std::string;

/** 未来予測アドバイザリー (spec/feature/flow/foresight.md) を求める観点タグ。付けた議論だけで走る。 */
inline constexpr std::string_view FORESIGHT_TAG = "未来予測";
/** 代理議論 (spec/feature/flow/proxy-debate.md) を求める観点タグ。 */
inline constexpr std::string_view PROXY_TAG = "代理";

bool wants_foresight(const std::vector<FlowTag>& tags);
bool wants_proxy(const std::vector<FlowTag>& tags);

/** 外部データ収集 (YouTube/Steam 等) が許可されるか。`機密` か `内部` が 1 つでもあれば false。 */
bool can_collect_external(const std::vector<FlowTag>& tags);

/**
 * 外部の声の検索を、タグが許す場合だけ渡す。
 * `機密` / `内部` では新規収集だけでなく KG に蓄積済みの外部の声も参照させない (nullopt を返す)。
 */
template <class T>
std::optional<T> gate_external_voices(const std::vector<FlowTag>& tags, std::optional<T> lookup) {
    if (!can_collect_external(tags)) return std::nullopt;
    return lookup;
}

/** タグに応じたディスカッションペーパーへの観点補足テキスト (複数タグは改行で連結)。 */
std::string paper_supplement(const std::vector<FlowTag>& tags);

} // namespace di::flow
