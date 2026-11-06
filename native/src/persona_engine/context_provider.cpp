#include "persona_engine/context_provider.hpp"

namespace di::persona_engine {

namespace {
Json opt(const std::optional<std::string>& v) { return v ? Json(*v) : Json(nullptr); }
} // namespace

Json to_json(const ContextHypothesis& h) {
    return Json{{"id", h.id}, {"statement", h.statement}, {"status", h.status}, {"designGapId", opt(h.designGapId)},
                {"updatedAt", h.updatedAt}};
}

Json to_json(const ContextGap& g) {
    return Json{{"id", g.id},
                {"title", g.title},
                {"description", opt(g.description)},
                {"status", g.status},
                {"gapIn", opt(g.gapIn)},
                {"expectedAffect", opt(g.expectedAffect)},
                {"observedAffect", opt(g.observedAffect)},
                {"hypothesisIds", g.hypothesisIds},
                {"updatedAt", g.updatedAt}};
}

Json to_json(const ContextUtterance& u) {
    return Json{{"id", u.id}, {"sessionId", u.sessionId}, {"rawContent", u.rawContent}, {"postedAt", u.postedAt}};
}

Json to_json(const ContextExternalVoice& v) {
    return Json{{"id", v.id},         {"content", v.content},           {"source", v.source},
                {"sourceUrl", opt(v.sourceUrl)}, {"speakerLabel", v.speakerLabel}, {"score", v.score}};
}

} // namespace di::persona_engine
