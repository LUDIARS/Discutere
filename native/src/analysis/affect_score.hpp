#pragma once
// src/analysis/affect-score.ts の移植: @ludiars/sentiment-core の affect scoring を旧 import 先から使えるようにする。
#include "lib/sentiment_core/affect_score.hpp"

namespace di::analysis {

using lib::sentiment_core::AffectScore;
using lib::sentiment_core::aspects;
using lib::sentiment_core::emotions;
using lib::sentiment_core::score_text;
using lib::sentiment_core::ValenceLabel;

} // namespace di::analysis
