#pragma once
// 値ではなく添字を並べ替えてから並べ直す安定ソート。
// std::sort / std::stable_sort に文字列を含む値を直接渡すと、Praesidium の静的解析 (clang --analyze) が
// MSVC の標準ライブラリの並べ替え実装の中の移動を「移動済みの値の使用」と誤検知する。文字列を含む
// 値の並べ替えはこちらを使う。比較は値への const 参照で行い、要素の移動は並べ替えの後に 1 回ずつ。
// 結果は std::stable_sort と同じ (等しい要素は元の順序を保つ)。
#include <algorithm>
#include <cstddef>
#include <numeric>
#include <utility>
#include <vector>

namespace di {

template <class T, class Less>
void stable_sort_by_index(std::vector<T>& values, Less less) {
    std::vector<std::size_t> order(values.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::stable_sort(order.begin(), order.end(),
                     [&values, &less](std::size_t a, std::size_t b) { return less(values[a], values[b]); });
    std::vector<T> sorted;
    sorted.reserve(values.size());
    for (const std::size_t i : order) sorted.push_back(std::move(values[i]));
    values = std::move(sorted);
}

} // namespace di
