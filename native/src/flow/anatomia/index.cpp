#include "flow/anatomia/index.hpp"

#include "base/strings.hpp"

namespace di::flow::anatomia {

std::vector<MechanicSummary> get_anatomia_mechanics(const AnatomiaSource& source, const GetAnatomiaMechanicsDeps& deps) {
    FetchAnatomiaDomainsOpts o;
    o.binPath = deps.binPath;
    if (deps.autoDraft) o.autoDraft = *deps.autoDraft;
    if (deps.timeout) o.timeout = *deps.timeout;
    o.runner = deps.runner;
    o.warn = deps.warn;
    const auto cards = fetch_anatomia_domains(source, o);
    RefineDomainsDeps r;
    r.llm = deps.llm;
    r.model = deps.refineModel;
    r.theme = deps.theme;
    r.warn = deps.warn;
    return refine_domains_to_mechanics(cards, r);
}

std::optional<AnatomiaSource> resolve_anatomia_source(const std::optional<std::string>& project, const std::optional<std::string>& repo_path) {
    if (project) {
        const std::string p = str::trim(*project);
        if (!p.empty()) return AnatomiaSource::project(p);
    }
    if (repo_path) {
        const std::string r = str::trim(*repo_path);
        if (!r.empty()) return AnatomiaSource::repo_path(r);
    }
    return std::nullopt;
}

} // namespace di::flow::anatomia
