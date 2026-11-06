#include "flow/design_gap.hpp"

#include <algorithm>

#include "base/fsutil.hpp"
#include "base/json.hpp"
#include "base/strings.hpp"
#include "discatier_engine_adapter/game_aliases.hpp"
#include "discatier_engine_adapter/ludus_term_aliases.hpp"
#include "flow/sentiment_vector.hpp"
#include "lib/front_matter/front_matter.hpp"

namespace di::flow {
namespace {

std::size_t dim_index(const std::string& name) {
    const auto& dims = vector_dims();
    return static_cast<std::size_t>(std::find(dims.begin(), dims.end(), name) - dims.begin());
}

/** *.sentiment.json の JSON 上限 (investigate と同じファイル上限 + 要素数)。 */
const JsonLimits kSentimentJsonLimits{kMaxSentimentFileBytes, 5'000'000, 64};

/**
 * gamesDir からゲームタイトル群を集める (別名グループ構築の材料)。
 *   - `*.md` の frontmatter title / `*.sentiment.json` の "game" フィールド
 * 読めないファイルは無視する (best-effort)。
 */
std::vector<std::string> collect_game_titles(const std::string& games_dir) {
    std::vector<std::string> titles;
    for (const auto& f : fs::list_dir(games_dir)) {
        const std::string full = fs::join(games_dir, f);
        if (str::ends_with(f, ".sentiment.json")) {
            auto text = fs::read_file(full, kMaxSentimentFileBytes);
            if (!text) continue;
            auto data = parse_json(*text, kSentimentJsonLimits);
            if (!data || !data->is_object()) continue;
            if (data->contains("game") && (*data)["game"].is_string()) {
                const std::string t = str::trim((*data)["game"].get<std::string>());
                if (!t.empty()) titles.push_back(t);
            }
        } else if (str::ends_with(f, ".md")) {
            auto text = fs::read_file(full, kMaxGameFileBytes);
            if (!text) continue;
            auto m = lib::front_matter::parse(*text);
            if (!m) continue;
            if (m->data.contains("title") && m->data["title"].is_string()) {
                const std::string t = str::trim(m->data["title"].get<std::string>());
                if (!t.empty()) titles.push_back(t);
            }
        }
    }
    return titles;
}

/** theme.toLowerCase().split(/\s+/).filter(w => w.length >= 2) */
std::vector<std::string> theme_words(const std::string& theme) {
    const std::string lower = str::to_lower(theme);
    std::vector<std::string> out;
    std::string cur;
    std::size_t i = 0;
    const auto flush = [&] {
        if (str::js_length(cur) >= 2) out.push_back(cur);
        cur.clear();
    };
    while (i < lower.size()) {
        const std::size_t start = i;
        const auto cp = str::next_codepoint(lower, i);
        if (str::is_js_space(cp)) flush();
        else cur.append(lower, start, i - start);
    }
    flush();
    return out;
}

} // namespace

std::vector<double> negative_baseline_vector() {
    std::vector<double> v(DIM, 0.5);
    v[dim_index("emo.valence")] = 0.2;  // negative valence
    v[dim_index("meta.positive_ratio")] = 0.2;
    v[dim_index("meta.volume_log")] = 0;
    return v;
}

std::optional<std::vector<double>> load_current_vector(const std::string& theme, const std::string& games_dir,
                                                       const LoadCurrentVectorOpts& opts) {
    const auto warn = [&](const std::string& m) {
        if (opts.warn) opts.warn(m);
    };
    if (!fs::exists(games_dir)) {
        warn("現状ベクトル: gamesDir が存在しません (" + games_dir + ")");
        return std::nullopt;
    }
    std::vector<std::string> files;
    for (const auto& f : fs::list_dir(games_dir))
        if (str::ends_with(f, ".sentiment.json")) files.push_back(f);
    const auto terms = discatier_engine_adapter::expand_aliases(
        theme_words(theme),
        discatier_engine_adapter::build_alias_groups(collect_game_titles(games_dir), discatier_engine_adapter::ludus_term_alias_groups()));

    for (const auto& f : files) {
        const std::string lower = str::to_lower(f);
        if (std::none_of(terms.begin(), terms.end(), [&](const std::string& t) { return str::contains(lower, t); })) continue;
        auto text = fs::read_file(fs::join(games_dir, f), kMaxSentimentFileBytes);
        if (!text) continue;
        auto data = parse_json(*text, kSentimentJsonLimits);
        if (!data || !data->is_object() || !data->contains("game_vector")) continue;
        const Json& gv = (*data)["game_vector"];
        if (!gv.is_array() || gv.size() != DIM) continue;
        std::vector<double> out;
        bool numeric = true;
        for (const auto& x : gv) {
            if (!x.is_number()) {
                numeric = false;
                break;
            }
            out.push_back(x.get<double>());
        }
        if (numeric) return out;
    }
    warn("現状ベクトル: テーマ「" + theme + "」に一致する sentiment.json が見つかりません" + " (検索語: " +
         (terms.empty() ? std::string("(なし)") : str::join(terms, ", ")) + ")");
    return std::nullopt;
}

std::vector<double> build_target_vector(const std::vector<MechanicSummary>& mechanics) {
    std::vector<double> target(DIM, 0.5);
    const auto& emo = emo_keys();
    const auto& asp = asp_keys();
    for (const auto& m : mechanics) {
        for (const auto& a : m.intended_aspects.value_or(std::vector<std::string>{})) {
            auto it = std::find(asp.begin(), asp.end(), a);
            if (it != asp.end()) target[2 + emo.size() + static_cast<std::size_t>(it - asp.begin())] = 1;
        }
        for (const auto& e : m.intended_emotions.value_or(std::vector<std::string>{})) {
            auto it = std::find(emo.begin(), emo.end(), e);
            if (it != emo.end()) target[2 + static_cast<std::size_t>(it - emo.begin())] = 1;
        }
    }
    // valence: intended_valence の集計 (positive=+1 / negative=-1) を 0..1 に写像
    double sum = 0, n = 0;
    for (const auto& m : mechanics) {
        if (!m.intended_valence) continue;
        sum += *m.intended_valence == "positive" ? 1 : *m.intended_valence == "negative" ? -1 : 0;
        n += 1;
    }
    const std::size_t valence = dim_index("emo.valence");
    if (n > 0) target[valence] = (sum / n + 1) / 2;
    else target[valence] = 1;  // intended は基本 positive 体験を狙う
    return target;
}

std::vector<double> compute_design_gap(const std::vector<double>& current, const std::vector<double>& target) {
    return subtract(target, current);
}

} // namespace di::flow
