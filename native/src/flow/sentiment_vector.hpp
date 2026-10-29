#pragma once
// src/flow/sentiment-vector.ts の移植: 20 次元の感情ベクトル (@ludiars/sentiment-core) を flow から使う入口。
#include "lib/sentiment_core/vector.hpp"

namespace di::flow {

using lib::sentiment_core::asp_keys;
using lib::sentiment_core::cosine;
using lib::sentiment_core::DIM;
using lib::sentiment_core::dot;
using lib::sentiment_core::emo_keys;
using lib::sentiment_core::norm;
using lib::sentiment_core::scalar_projection;
using lib::sentiment_core::subtract;
using lib::sentiment_core::text_to_vector;
using lib::sentiment_core::Vector;
using lib::sentiment_core::VECTOR_SPEC_VERSION;
using lib::sentiment_core::vector_dims;
using lib::sentiment_core::vector_spec;
using lib::sentiment_core::weighted_mean;

} // namespace di::flow
