#pragma once
// w2g_db_tests.cpp を Praesidium の静的解析 (1 単位 60 秒) に収まるよう分けた各パートが共有する準備。
// w2-G: DB を使う部品 (ペーパーの永続化・版履歴・声の保存と使い回し・議論一覧・削除・Discord の再議論・
// 弁証法 4 表・コストログ・games md) と、入力経路の境界 (上限ちょうど・上限+1・壊れた入力・巨大な入力)。
// TS の tests/flow/{paper-revisions,paper-reuse,foundation,cost-instrumentation}.test.ts の観点を移した。
#include "flow/w2g_support.hpp"

#include <chrono>

#include "base/strings.hpp"
#include "flow/cost_logger.hpp"
#include "flow/dialectic/store.hpp"
#include "flow/discord_flow_runs.hpp"
#include "flow/discussion_paper.hpp"
#include "flow/effect_predict.hpp"
#include "flow/flow_setup.hpp"
#include "flow/games_md.hpp"
#include "flow/mechanic_extract.hpp"
#include "flow/paper_markdown.hpp"
#include "flow/paper_reuse.hpp"
#include "flow/paper_revisions.hpp"
#include "flow/paper_voices.hpp"
#include "flow/spec_analyze.hpp"
#include "lib/front_matter/front_matter.hpp"

using namespace di;
using namespace di::flow;
using w2g::FakeLLM;

namespace w2g_db_support {
NewPaper new_paper(const std::string& sid, const std::string& theme, std::optional<std::string> body = std::nullopt) {
    return {sid, theme, {"開発"}, {MechanicSummary{"M", "d", {}, {}, {}, {}, std::string("curated")}}, "補足", std::move(body)};
}
Json ok(const std::string& t) { return Json{{"ok", true}, {"text", t}}; }
} // namespace w2g_db_support
