#pragma once
// 画面 (native/web/**) を引く補助。TS が返していた HTML 文字列は native/web に置いてある
// (native/tests/golden/w1-F-golden.gen.ts が Node の出力をそのまま書き出したもの)。
#include <stdexcept>
#include <string>
#include <string_view>

#include "base/web_assets.hpp"

namespace di::api {

/** 埋め込み資産の本文。無ければ std::logic_error (ビルドの不備なので起動時に気づく)。 */
inline std::string_view web_page(std::string_view name) {
    const auto* asset = web::find(name);
    if (!asset) throw std::logic_error("web asset not embedded: " + std::string(name));
    return asset->body;
}

} // namespace di::api
