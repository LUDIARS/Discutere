#include "flow/persona_descriptor.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>
#include <vector>

#include "base/json.hpp"
#include "base/strings.hpp"
#include "visualize/locale_compare.hpp"

namespace di::flow {
namespace {

// @spec 憑依 descriptor の強化
const std::map<std::string, std::string>& axis_labels() {
    static const std::map<std::string, std::string> labels = {
        {"mtg.timmy", "大きな体験"},
        {"mtg.johnny", "独自の工夫"},
        {"mtg.spike", "勝利と最適化"},
        {"style.achiever", "達成"},
        {"style.explorer", "探索"},
        {"style.socializer", "交流"},
        {"style.competitor", "競争"},
        {"style.collector", "収集"},
        {"style.narrative", "物語"},
        {"style.relaxation", "くつろぎ"},
        {"style.mastery", "上達"},
        {"style.onboarding_need", "丁寧な導入"},
        {"style.autonomy", "自由度"},
        {"style.routine_tolerance", "反復"},
        {"style.monetization_sensitivity", "課金への慎重さ"},
    };
    return labels;
}

/** item.target.replace(/^mechanic:/, "") */
std::string strip_mechanic_prefix(const std::string& s) {
    return str::starts_with(s, "mechanic:") ? s.substr(9) : s;
}

} // namespace

std::string persona_axis_label(const std::string& axis) {
    const auto& labels = axis_labels();
    auto it = labels.find(axis);
    return it == labels.end() ? axis : it->second;
}

std::string build_persona_descriptor(const PoolPersona& persona) {
    std::vector<std::string> parts;

    std::vector<std::pair<std::string, double>> axes;
    for (const auto& a : persona.preferenceAxes)
        if (std::isfinite(a.second)) axes.push_back(a);
    std::stable_sort(axes.begin(), axes.end(), [](const auto& left, const auto& right) {
        if (right.second != left.second) return right.second < left.second;
        return visualize::locale_compare(left.first, right.first) < 0;
    });
    if (!axes.empty()) {
        std::vector<std::string> high;
        for (std::size_t i = 0; i < axes.size() && i < 2; ++i) high.push_back(persona_axis_label(axes[i].first));
        parts.push_back(str::join(high, "と") + "を強く好む");
        const std::string low = persona_axis_label(axes.back().first);
        if (std::find(high.begin(), high.end(), low) == high.end()) parts.push_back(low + "は好まない");
    }

    auto aversions = persona.aversions;
    std::stable_sort(aversions.begin(), aversions.end(), [](const PersonaAversion& left, const PersonaAversion& right) {
        if (right.strength != left.strength) return right.strength < left.strength;
        return visualize::locale_compare(left.target, right.target) < 0;
    });
    std::vector<std::string> aversion_names;
    for (std::size_t i = 0; i < aversions.size() && i < 2; ++i) aversion_names.push_back(strip_mechanic_prefix(aversions[i].target));
    if (!aversion_names.empty()) parts.push_back(str::join(aversion_names, "・") + "を嫌う");

    std::vector<std::string> attributes;
    if (persona.attributes.ageBand && !persona.attributes.ageBand->empty()) attributes.push_back(*persona.attributes.ageBand);
    if (persona.attributes.spending && !persona.attributes.spending->empty()) attributes.push_back(*persona.attributes.spending);
    if (!attributes.empty()) parts.push_back(str::join(attributes, " / "));

    auto reactions = persona.mechanicReactions;
    std::stable_sort(reactions.begin(), reactions.end(), [](const PersonaMechanicReaction& left, const PersonaMechanicReaction& right) {
        const double l = std::fabs(left.sentiment), r = std::fabs(right.sentiment);
        if (r != l) return r < l;
        return visualize::locale_compare(left.mechanicId, right.mechanicId) < 0;
    });
    std::vector<std::string> reaction_texts;
    for (std::size_t i = 0; i < reactions.size() && i < 2; ++i)
        reaction_texts.push_back(reactions[i].mechanicId + (reactions[i].sentiment >= 0 ? "に強く反応" : "が苦手"));
    if (!reaction_texts.empty()) parts.push_back(str::join(reaction_texts, "、"));

    if (parts.empty()) {
        if (!persona.traits.empty()) parts.push_back("特徴: " + str::join(persona.traits, " / "));
        if (!persona.speechStyle.empty()) parts.push_back("話し方: " + persona.speechStyle);
        if (persona.learningSource && !persona.learningSource->empty()) parts.push_back("出所: " + *persona.learningSource);
        if (persona.typicality) parts.push_back(*persona.typicality >= 0.9 ? "多数派寄りの感性" : "やや独自の感性");
        if (persona.polarityBias)
            parts.push_back(*persona.polarityBias >= 0.5 ? "好き嫌いがはっきりしている" : "是々非々で評価する");
        if (persona.affectDispersion && *persona.affectDispersion >= 0.1) parts.push_back("ゲームによって評価が大きく変わる");
    }

    return !parts.empty() ? "この人物像は実データ由来。" + str::join(parts, "、") + "。この感性になりきって発言する。"
                          : "この人物像は実データ由来の感性を持つ。その立場になりきって発言する。";
}

} // namespace di::flow
