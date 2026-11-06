#pragma once
// fetch_tests.cpp を Praesidium の静的解析 (1 単位 60 秒) に収まるよう分けた各パートが共有する準備。
// crawler/sources と Voluptas client のネットワーク部分を偽サーバで確かめる
// (tests/crawler/external/{youtube-fetch,website}.test.ts, tests/flow/user-voices.test.ts の Voluptas 部分の観点 + 境界)。
#include <atomic>
#include <mutex>

#include "base/strings.hpp"
#include "check.hpp"
#include "crawler/sources/niconico.hpp"
#include "crawler/sources/reddit.hpp"
#include "crawler/sources/steam.hpp"
#include "crawler/sources/website.hpp"
#include "crawler/sources/youtube_comments.hpp"
#include "crawler/sources/youtube_videos.hpp"
#include "flow/user_voices/voluptas_impressions.hpp"
#include "w1d_support.hpp"

using namespace di;
using namespace di::crawler::sources;

namespace fetch_support {
http::Response json_res(const Json& j, int status = 200) { return http::Response::json(j, status); }
} // namespace fetch_support
