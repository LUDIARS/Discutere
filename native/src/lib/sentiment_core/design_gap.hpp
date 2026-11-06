#pragma once
// @ludiars/sentiment-core design-gap.ts の移植: メカニクスの狙いから目標ベクトルを作り、現在との差を取る。
#include <optional>
#include <string>
#include <vector>

#include "lib/sentiment_core/vector.hpp"

namespace di::lib::sentiment_core {

struct MechanicIntent {
    std::optional<std::vector<std::string>> intended_aspects;
    std::optional<std::vector<std::string>> intended_emotions;
    std::optional<std::string> intended_valence;
};

Vector build_target_vector(const std::vector<MechanicIntent>& mechanics);
/** target - current (次元が違えば std::invalid_argument)。 */
Vector compute_design_gap(const Vector& current, const Vector& target);

} // namespace di::lib::sentiment_core
