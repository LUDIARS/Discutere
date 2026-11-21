#pragma once
#include "playtest/contracts/runtime.hpp"

namespace di::playtest::checks {
inline const auto seed = augur::post_only([](bool result, auto count, auto minimum) {
    return result == (minimum > 0 && count >= minimum);
});
}
