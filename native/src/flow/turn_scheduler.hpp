#pragma once
// src/flow/turn-scheduler.ts の移植: シャッフル輪番 (respec 02, A2)。
// ラウンド開始時に facilitator を除くペルソナ列をシャッフルしてその順に巡回する。
// 純関数 (Rng 注入で決定的)。
#include <cmath>
#include <utility>
#include <vector>

#include "flow/personas.hpp"

namespace di::flow {

/** Fisher–Yates シャッフル (非破壊・rng 注入で決定的)。 */
template <class T>
std::vector<T> shuffle(const std::vector<T>& items, const Rng& rng) {
    std::vector<T> arr = items;
    for (std::size_t i = arr.size(); i-- > 1;) {
        double f = std::floor(rng() * static_cast<double>(i + 1));
        // rng が [0,1) を外れても範囲外を引かない
        if (!(f >= 0)) f = 0;
        if (f > static_cast<double>(i)) f = static_cast<double>(i);
        const auto j = static_cast<std::size_t>(f);
        std::swap(arr[i], arr[j]);
    }
    return arr;
}

/**
 * ラウンドの発言順を組み立てる (シャッフル輪番)。
 * facilitator は除外。turnsPerRound > 人数 のときは周回ごとに再シャッフルし、
 * 周回境界で前周の末尾と次周の先頭が同一なら swap する。
 * @returns 長さ turnsPerRound の発言者列 (発言者が 0 人なら空)。
 */
std::vector<FlowPersona> build_turn_order(const std::vector<FlowPersona>& personas, int turns_per_round, const Rng& rng);

} // namespace di::flow
