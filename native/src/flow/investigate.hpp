#pragma once
// src/flow/investigate.ts の移植: テーマ調査 (discussion.md step 2)。
// メカニクス + 感情データを収集し、0 件の場合は YouTube で補完する。
// can_collect_external が false (機密/内部) なら外部取得しない。
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "base/json.hpp"
#include "flow/tags.hpp"

namespace di::flow {

/** メカニクスの出所ラベル: "curated" | "llm" | "crawl"。 */
using MechanicSource = std::string;

/** 文字列を MechanicSource に正規化する (不正/未指定は nullopt = 旧データ = curated 扱い)。 */
std::optional<MechanicSource> as_mechanic_source(const Json& v);

struct MechanicSummary {
    std::string name;
    std::string description;
    std::optional<std::string> intended_affect;
    std::optional<std::string> intended_valence;
    std::optional<std::vector<std::string>> intended_aspects;
    std::optional<std::vector<std::string>> intended_emotions;
    std::optional<MechanicSource> source;
};

/** loadThemeMechanics の出力と同じキー順 (name, description, source, intended_*)。 */
Json to_json(const MechanicSummary& m);
MechanicSummary mechanic_summary_from_json(const Json& j);

struct InvestigateResult {
    std::vector<MechanicSummary> mechanics;
    double sentimentCount = 0;
    bool youtubeUsed = false;
    std::vector<std::string> youtubeComments;
};

/** YouTube 検索 (注入式)。失敗は例外。 */
using YoutubeSearchFn = std::function<std::vector<std::string>(const std::string& query, int max_comments)>;

struct InvestigateArgs {
    std::string theme;
    std::vector<FlowTag> tags;
    std::string gamesDir = "./data/games";
    YoutubeSearchFn youtubeSearch;
    int youtubeMaxComments = 200;
    std::function<void(const std::string&)> warn;
    std::function<double(const std::string&)> getSentimentCount;
};

/** data/games の 1 ファイルの上限 (外部入力として読む)。 */
inline constexpr std::size_t kMaxGameFileBytes = 4u << 20;
/** *.sentiment.json の 1 ファイルの上限。 */
inline constexpr std::size_t kMaxSentimentFileBytes = 64u << 20;

/**
 * gamesDir から、テーマに明確に紐づくゲームのメカニクスだけを読む (最大 10 件、source は frontmatter 優先・無ければ curated)。
 * 外部取得はしない。読めない front matter のファイルは飛ばす (warn に理由)。
 */
std::vector<MechanicSummary> load_theme_mechanics(const std::string& games_dir, const std::string& theme,
                                                  const std::function<void(const std::string&)>& warn = {});

InvestigateResult investigate_theme(const InvestigateArgs& args);

} // namespace di::flow
