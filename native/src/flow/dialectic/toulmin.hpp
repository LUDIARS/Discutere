#pragma once
// トゥールミンモデルの 6 要素の表示 (dialectic.md §2.5、neco「トゥールミンモデルを正しく適用」2026-11-23)。
//
//   主張 (claim) / データ (data = Ground.text) / 論拠 (warrant) / 裏付け (backing: 論拠がなぜ成り立つか)
//   限定 (qualifier: 範囲 = どの層・どの条件の話か + 確度 = 必ず / 多くの場合 / 条件つきで / おそらく)
//   反駁 (rebuttal: 主張する本人が挙げる「この主張が成り立たない場合 = 例外条件」)
// 相手の反論 (応酬の rebut) は Toulmin の反駁ではない。相手は例外条件を「突く先」(attack=exception) に選べる。
// 判定・応酬・止揚・決着表が同じ見せ方をするよう、表示はここに集める。
#include <string>

#include "flow/dialectic/store.hpp"

namespace di::flow::dialectic {

/** 限定の 1 行 (「範囲: … / 確度: …」、どちらも無ければ「(明示なし)」)。 */
std::string render_qualifier(const PositionRecord& p);

/** 例外条件の 1 行 (「例外: A / B」、無ければ空)。 */
std::string render_exceptions(const PositionRecord& p);

/** 根拠 1 件の論拠・裏付けの行 (字下げ付き、無い要素は出さない)。 */
std::string render_warrant_lines(const Ground& g, const std::string& indent);

} // namespace di::flow::dialectic
