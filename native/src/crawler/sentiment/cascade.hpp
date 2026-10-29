#pragma once
// src/crawler/sentiment/cascade.ts の移植: @ludiars/sentiment-core の cascade を旧 import 先から使えるようにする。
#include "lib/sentiment_core/cascade.hpp"

namespace di::crawler::sentiment {

using lib::sentiment_core::cascade_sentiment;
using lib::sentiment_core::CascadeClients;
using lib::sentiment_core::Polarity;
using lib::sentiment_core::SentimentResult;
using lib::sentiment_core::Signal;

} // namespace di::crawler::sentiment
