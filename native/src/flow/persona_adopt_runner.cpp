#include "flow/persona_adopt_runner.hpp"

#include <unordered_map>

#include "base/strings.hpp"
#include "config.hpp"
#include "core/index.hpp"
#include "core/kg_registry.hpp"

namespace di::flow {

namespace {

template <class T>
std::vector<std::pair<std::string, double>> tally_reasons(const std::vector<T>& rejected) {
    std::vector<std::pair<std::string, double>> out;
    for (const auto& r : rejected) {
        bool found = false;
        for (auto& [reason, n] : out)
            if (reason == r.reason) {
                n += 1;
                found = true;
                break;
            }
        if (!found) out.emplace_back(r.reason, 1);
    }
    return out;
}

} // namespace

Json to_json(const AdoptRunSummary& s) {
    Json reasons = Json::object();
    for (const auto& [k, v] : s.rejectedByReason) reasons[k] = v;
    return Json{{"speakers", s.speakers},
                {"utterances", s.utterances},
                {"adopted", s.adopted},
                {"rejected", s.rejected},
                {"rejectedByReason", std::move(reasons)}};
}

std::vector<SpeakerOpinions> collect_speaker_opinions(sql::Database& raw, crawler::sources::AttributionStore& attribution,
                                                      const std::string& workspace, const std::optional<std::string>& source_filter,
                                                      double* utterance_count) {
    const auto rows = raw.all("SELECT id, speaker_id, raw_content FROM utterances WHERE workspace_id = ? AND speaker_id LIKE 'ext:%'",
                              {workspace}, kAdoptKgReadLimits);
    // opinion-score (#125 (a)): 発話ごとの positive リアクション (いいね) 合算を weight に使う。
    const auto score_rows = raw.all(
        "SELECT utterance_id, SUM(intensity) AS likes FROM reactions WHERE workspace_id = ? AND reaction_type = 'positive' GROUP BY utterance_id",
        {workspace}, kAdoptKgReadLimits);
    std::unordered_map<std::string, double> likes_by_id;
    for (const auto& r : score_rows) likes_by_id[r.str("utterance_id")] = r.num("likes", 0);

    std::vector<SpeakerOpinions> speakers;
    std::unordered_map<std::string, std::size_t> index;
    for (const auto& r : rows) {
        const auto id = r.str("id");
        const auto speaker_id = r.str("speaker_id");
        const auto attr = attribution.get(id);
        std::optional<std::string> source;
        if (attr) {
            source = attr->source;
        } else {
            const auto parts = str::split(speaker_id, ":");
            if (parts.size() > 1) source = parts[1];
        }
        if (source_filter && source != *source_filter) continue;
        auto it = index.find(speaker_id);
        if (it == index.end()) {
            it = index.emplace(speaker_id, speakers.size()).first;
            speakers.push_back(SpeakerOpinions{speaker_id, source, {}});
        }
        // weight = 1 + いいね数 (0 いいねでも基準 1 で残す)。
        const auto like = likes_by_id.find(id);
        SpeakerOpinion op;
        op.text = r.str("raw_content");
        op.gameSlug = attr ? attr->gameSlug : std::nullopt;
        op.weight = 1 + (like == likes_by_id.end() ? 0.0 : like->second);
        speakers[it->second].opinions.push_back(std::move(op));
    }
    if (utterance_count) *utterance_count = static_cast<double>(rows.size());
    return speakers;
}

AdoptRunSummary adopt_from_speakers(const std::vector<SpeakerOpinions>& speakers, double utterance_count, const AdoptRunOptions& opts) {
    const AdoptOptions& adopt_opts = opts;
    AdoptRunSummary s;
    s.speakers = static_cast<double>(speakers.size());
    s.utterances = utterance_count;
    if (opts.dry) {
        const auto res = evaluate_speakers(speakers, adopt_opts);
        s.adopted = static_cast<double>(res.candidates.size());
        s.rejected = static_cast<double>(res.rejected.size());
        s.rejectedByReason = tally_reasons(res.rejected);
        return s;
    }
    const auto res = adopt_personas(speakers, adopt_opts);
    s.adopted = static_cast<double>(res.adopted.size());
    s.rejected = static_cast<double>(res.rejected.size());
    s.rejectedByReason = tally_reasons(res.rejected);
    return s;
}

AdoptRunSummary run_adopt_from_kg(const AdoptRunOptions& opts) {
    const auto& cfg = config::get_config();
    core::CoreOptions co;
    co.sqlitePath = core::resolve_active_kg_path(cfg);
    auto core = core::create_core(co);
    auto attribution = crawler::sources::open_attribution_store();
    struct Closer {
        core::Core& c;
        crawler::sources::AttributionStore& a;
        ~Closer() {
            try {
                a.close();
            } catch (...) {
            }
            try {
                c.close();
            } catch (...) {
            }
        }
    } closer{*core, *attribution};
    double utterances = 0;
    const auto speakers = collect_speaker_opinions(core->raw(), *attribution, cfg.workspace, opts.sourceFilter, &utterances);
    return adopt_from_speakers(speakers, utterances, opts);
}

} // namespace di::flow
