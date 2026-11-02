#include "flow/foresight/after_flow.hpp"

#include <stdexcept>

#include "base/log.hpp"
#include "base/strings.hpp"
#include "config.hpp"
#include "flow/discussion_paper.hpp"
#include "flow/foresight/markdown.hpp"
#include "flow/foresight/phase.hpp"
#include "flow/foresight/prompts.hpp"
#include "flow/foresight/state.hpp"

namespace di::flow::foresight {

bool foresight_requested(const std::vector<FlowTag>& tags) {
    return config::get_config().flow.foresight.enabled && wants_foresight(tags);
}

FlowDirectorResult run_with_foresight(const std::string& theme, const std::vector<FlowTag>& tags, const std::string& flow,
                                      const std::function<FlowDirectorResult()>& run, const ForesightAfterFlowDeps& deps) {
    const bool requested = foresight_requested(tags);
    if (requested && deps.sessionId) set_foresight_status(*deps.sessionId, "pending");
    FlowDirectorResult result;
    try {
        result = run();
    } catch (...) {
        if (requested && deps.sessionId) set_foresight_status(*deps.sessionId, "failed", "本議論が例外で止まった");
        throw;
    }
    return continue_with_foresight(theme, tags, flow, std::move(result), deps);
}

namespace {

/** 本議論の文脈 (ペーパー本文 + 外部の声)。取れなければ空で続ける。 */
ForesightContext build_context(const std::string& theme, const std::string& session_id, const ForesightAfterFlowDeps& deps,
                               const WarnFn& warn) {
    ForesightContext ctx;
    try {
        ctx.paperMd = strip_foresight(get_paper_body_by_session(session_id).value_or(""));
    } catch (const std::exception& e) {
        warn(std::string("未来予測: ペーパー本文を読めなかった (") + e.what() + ")");
    }
    if (deps.listExternalVoices) {
        try {
            for (const auto& v : deps.listExternalVoices({theme}, static_cast<int>(CONTEXT_VOICE_LIMIT))) {
                const std::string content =
                    str::js_length(v.content) > VOICE_CHARS ? str::js_slice(v.content, 0, static_cast<std::int64_t>(VOICE_CHARS)) + "…" : v.content;
                ctx.voices.push_back({content, v.source});
            }
        } catch (const std::exception& e) {
            warn(std::string("未来予測: 外部の声を引けなかった (") + e.what() + ")");
        }
    }
    return ctx;
}

} // namespace

FlowDirectorResult continue_with_foresight(const std::string& theme, const std::vector<FlowTag>& tags, const std::string& flow,
                                           FlowDirectorResult result, const ForesightAfterFlowDeps& deps) {
    if (!foresight_requested(tags)) return result;
    if (!deps.llm) throw std::invalid_argument("continue_with_foresight: llm is required");
    const auto& cfg = config::get_config().flow.foresight;
    const WarnFn log = deps.log ? deps.log : WarnFn([](const std::string& m) { di::log::info("[flow/foresight] " + m); });
    const WarnFn warn = deps.warn ? deps.warn : WarnFn([](const std::string& m) { di::log::warn("[flow/foresight/warn] " + m); });
    const std::string session_id = result.sessionId;

    // 結論の無い議論から未来は予測しない (土台が無いまま一手を作らせない。2026-10-18 Astra 評価)。
    if (!result.concluded || str::trim(result.conclusion).empty()) {
        log("未来予測: 本議論が収束していないため行わない");
        set_foresight_status(session_id, "skipped", "本議論が収束していない");
        return result;
    }

    set_foresight_status(session_id, "running");
    try {
        ForesightPhaseArgs pa;
        pa.theme = theme;
        pa.sessionId = session_id;
        pa.paperId = result.paperId;
        pa.flow = flow;
        pa.utterances = result.utterances;
        pa.conclusion = result.conclusion;
        pa.context = build_context(theme, session_id, deps, warn);
        pa.turns = cfg.turns;
        if (!cfg.model.empty()) pa.model = cfg.model;
        if (!cfg.forecastModel.empty())
            pa.forecastModel = cfg.forecastModel;
        else if (!cfg.model.empty())
            pa.forecastModel = cfg.model;
        pa.commit = [&deps](const FlowUtteranceRecord& record) {
            persist_utterance(record);
            if (deps.onUtterance) deps.onUtterance(record);
        };
        const std::string paper_id = result.paperId;
        pa.paperBody = PaperBodyAccess{[session_id] { return get_paper_body_by_session(session_id); },
                                       [paper_id](const std::string& body) { update_paper_body(paper_id, body); }};
        pa.log = log;
        pa.warn = warn;
        const auto phase = run_foresight_phase(*deps.llm, pa);
        if (!phase) {
            set_foresight_status(session_id, "skipped", "改革派と保守派に分けられる話者がいない");
            return result;
        }
        set_foresight_status(session_id, phase->status,
                             phase->status == "done" ? std::string() : "失敗した段 " + std::to_string(phase->failures) + " 件");
        result.utterances.insert(result.utterances.end(), phase->utterances.begin(), phase->utterances.end());
        result.foresight = FlowDirectorResult::Foresight{phase->proposal, phase->forecast};
        return result;
    } catch (const std::exception& e) {
        warn(std::string("未来予測を中断 (本議論の結果は維持): ") + e.what());
        set_foresight_status(session_id, "failed", std::string("例外: ") + e.what());
        return result;
    }
}

} // namespace di::flow::foresight
