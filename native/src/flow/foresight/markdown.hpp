#pragma once
// 未来予測アドバイザリーの結果をペーパー本文 (body_md) の末尾節にする (pure。src/flow/foresight/markdown.ts の移植)。
// 既存の節は取り除いてから付け直す (冪等。やり直し時に重ならない)。
#include <string>
#include <string_view>

namespace di::flow::foresight {

struct ForesightSection {
    std::string proposal;
    std::string forecast;
};

/** body_md から未来予測の節以降を取り除く (現行と 2026-10-18 初版の見出しの両方)。 */
std::string strip_foresight(std::string_view body_md);
/** body_md の末尾に未来予測の節 (提案 + 予測) を付ける。 */
std::string append_foresight_markdown(std::string_view body_md, const ForesightSection& section);

} // namespace di::flow::foresight
