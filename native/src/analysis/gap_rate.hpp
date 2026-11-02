#pragma once
// Gap率アナライザ (read-only, src/analysis/gap-rate.ts)。
//
// 「運営の想定感情」(GameKG md の mechanic 単位 intended 定義) と「観測感情」(KG 発話を lexicon 採点) を突合し、
// 運営の意図と違う感情がどれだけ出ているか = Gap率 を 施策時系列 / ソース / 感情次元 / mechanic で算出する。
//   intended = data/games/<slug>.md  (mechanic.intended_valence / intended_aspects)
//   observed = KG utterances → score_text() で valence/aspect 採点
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "analysis/affect_score.hpp"
#include "base/json.hpp"

namespace di::analysis {

/** "match" | "offIntentPositive" | "unintendedNegative" | "blindSpot" | "neutral" */
using GapClass = std::string;

struct MechanicGap {
    std::string name;
    ValenceLabel intendedValence;
    std::vector<std::string> intendedAspects;
    double attributed = 0;
    double gapRate = 0;
    double negativeRate = 0;
};

struct SliceGap {
    std::string key;
    double n = 0;
    double gapRate = 0;
    double negativeRate = 0;
};

struct AspectGap {
    std::string aspect;
    bool intended = false;
    double mentions = 0;
    double negativeRate = 0;
};

struct GapBreakdown {
    double match = 0;
    double offIntentPositive = 0;
    double unintendedNegative = 0;
    double blindSpot = 0;
    double neutral = 0;
};

struct GapRateReport {
    std::string game;
    std::string slug;
    double total = 0;
    GapBreakdown breakdown;
    double matchRate = 0;
    double gapRate = 0;
    double neutralRate = 0;
    double negativeRate = 0;
    std::vector<SliceGap> bySource;
    std::vector<SliceGap> byPeriod;
    std::vector<AspectGap> byAspect;
    std::vector<MechanicGap> byMechanic;
};

Json to_json(const MechanicGap& v);
Json to_json(const SliceGap& v);
Json to_json(const AspectGap& v);
Json to_json(const GapRateReport& v);

struct IntendedMechanic {
    std::string name;
    ValenceLabel valence;
    std::vector<std::string> aspects;
};

struct IntendedDef {
    std::set<std::string> intendedAspects;
    std::vector<IntendedMechanic> mechanics;
};

struct ScoredUtterance {
    std::string source;
    std::string period;  // YYYY-MM
    AffectScore score;
    GapClass cls = "neutral";
};

/** GameKG md の上限。 */
inline constexpr std::size_t kMaxGameKgBytes = 8u << 20;

/** 既定の KG パス (active KG → DISCATIER_KUZU_PATH → ./data/discatier.kuzu)。 */
std::string default_gap_kuzu_path();

/**
 * GameKG md (運営の定義) を読み、intended プロファイルを組む。無ければ std::runtime_error (TS と同じ文言)。
 * slug は games_dir の下に収まること (".." や絶対パスは拒否。移植で追加)。
 */
IntendedDef load_intended(const std::string& slug, const std::optional<std::string>& games_dir = std::nullopt);

/** KG から該当ゲームの発話を取り出して採点する (session.title = `<source>:<slug>`)。DB は読み取り専用で開く。 */
std::vector<ScoredUtterance> load_observed(const std::string& slug, const std::optional<std::string>& db_path = std::nullopt);

GapRateReport analyze_gap_rate(const std::string& slug, const std::optional<std::string>& games_dir = std::nullopt,
                               const std::optional<std::string>& db_path = std::nullopt);

/** レポートを人間可読のテキストに整形する (CLI 用)。 */
std::string format_gap_report(const GapRateReport& r);

} // namespace di::analysis
