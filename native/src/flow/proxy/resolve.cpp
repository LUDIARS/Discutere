#include "flow/proxy/resolve.hpp"

#include <algorithm>

#include "base/time.hpp"
#include "config.hpp"
#include "discatier_engine_adapter/keyword_terms.hpp"
#include "flow/proxy/cast.hpp"

namespace di::flow::proxy {

std::optional<ProxyRun> resolve_proxy_run(const std::string& theme, const std::vector<FlowTag>& tags, std::optional<double> now) {
    if (!wants_proxy(tags)) return std::nullopt;
    const auto& cfg = config::get_config();
    if (!cfg.flow.proxy.enabled) throw ProxyRequestError("代理議論は無効になっています (flow.proxy.enabled=false)。");
    if (!can_collect_external(tags)) throw ProxyRequestError("機密 / 内部の議論では、公開レビューから作った代理は使えません。");
    const auto request = parse_proxy_request(theme);
    if (!request) throw ProxyRequestError("議題に「代理: 論者#xxxxxx, 論者#yyyyyy」を書いてください。");
    const std::string default_model = cfg.llm.model ? *cfg.llm.model : "claude-haiku-4-5-20251001";
    const std::vector<std::string> discussants =
        !cfg.flow.roster.discussants.empty() ? cfg.flow.roster.discussants : std::vector<std::string>{default_model};
    LoadProxyParticipantsArgs args;
    args.request = *request;
    args.now = now ? *now : static_cast<double>(time::now_ms());
    args.requirePassed = cfg.flow.proxy.requireEvalPassed;
    args.budgetTokens = cfg.flow.proxy.budgetTokens;
    args.models.assign(discussants.begin(), discussants.begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(2, discussants.size())));
    args.isLocal = cfg.llm.backend == "local";
    args.focusTerms = discatier_engine_adapter::extract_key_terms({request->theme});
    return ProxyRun{request->theme, load_proxy_participants(args)};
}

} // namespace di::flow::proxy
