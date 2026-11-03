#include "flow/dispatch.hpp"

#include <algorithm>

#include "base/strings.hpp"
#include "config.hpp"
#include "flow/dialectic/driver.hpp"
#include "flow/foresight/after_flow.hpp"
#include "flow/improvement.hpp"
#include "flow/proxy/resolve.hpp"

namespace di::flow {

namespace {

struct KindAliases {
    const char* kind;
    std::vector<std::string> aliases;
};

/** 議論タイプのラベル (WebUI select 値 / Discord タグ名) → FlowKind。 */
const std::vector<KindAliases>& flow_kind_aliases() {
    static const std::vector<KindAliases> table = {
        {"discussion", {"discussion", "企画/議論", "企画／議論", "議論", "ディスカッション", "討論"}},
        {"improvement", {"improvement", "改善/議論", "改善／議論", "改善", "改善提案", "提案"}},
        {"learning", {}},
        {"sparring", {}},
    };
    return table;
}

FlowDriverFn default_improvement_driver() {
    return [](persona_engine::LLMClient& llm, const std::string& theme, const std::vector<FlowTag>& tags, const FlowDirectorDeps& deps) {
        ImprovementFlowDeps d;
        static_cast<FlowDirectorDeps&>(d) = deps;
        return run_improvement_flow(llm, theme, tags, d);
    };
}

} // namespace

std::optional<FlowKind> parse_flow_kind(const std::optional<std::string_view>& label) {
    if (!label || label->empty()) return std::nullopt;
    const std::string l = str::to_lower(str::trim(*label));
    if (l.empty()) return std::nullopt;
    // 完全一致
    for (const auto& [kind, aliases] : flow_kind_aliases())
        for (const auto& a : aliases)
            if (str::to_lower(a) == l) return std::string(kind);
    // 部分一致 (フォーラムタグ名に装飾が付く場合)
    // 「改善/議論」を短い「議論」へ誤分類しないよう、具体的な別名を優先する。
    struct Match {
        const char* kind;
        std::string alias;
    };
    std::vector<Match> matches;
    for (const auto& [kind, aliases] : flow_kind_aliases())
        for (const auto& a : aliases) matches.push_back({kind, a});
    std::stable_sort(matches.begin(), matches.end(),
                     [](const Match& a, const Match& b) { return str::js_length(a.alias) > str::js_length(b.alias); });
    for (const auto& m : matches)
        if (str::contains(l, str::to_lower(m.alias))) return std::string(m.kind);
    return std::nullopt;
}

FlowTypeRequiredError::FlowTypeRequiredError(const std::string& label)
    : std::runtime_error("議論タイプが未指定または不正です: \"" + label + "\"。投稿は受理されません。") {}

DispatchResult dispatch_flow(const DispatchInput& input, const DispatchDeps& deps) {
    const auto kind = parse_flow_kind(std::string_view(input.flow));
    if (!kind) throw FlowTypeRequiredError(input.flow);
    if (!deps.llm) throw std::invalid_argument("dispatch_flow: llm is required");

    const std::string& theme = input.theme;
    const auto& tags = input.tags;
    // 機密/内部では蓄積済みの外部の声も使わない (本議論・壁打ち・未来予測の全経路)。
    const ListExternalVoicesFn list_external_voices =
        gate_external_voices(tags, deps.listExternalVoices ? std::optional<ListExternalVoicesFn>(deps.listExternalVoices) : std::nullopt)
            .value_or(ListExternalVoicesFn{});
    FlowDirectorDeps flow_deps;
    flow_deps.listExternalVoices = list_external_voices;
    flow_deps.youtubeSearch = deps.youtubeSearch;
    flow_deps.onUtterance = deps.onUtterance;
    flow_deps.onVote = deps.onVote;
    flow_deps.rng = deps.rng;
    flow_deps.gamesDir = deps.gamesDir;
    flow_deps.sessionId = deps.sessionId;
    flow_deps.paperOverride = deps.paperOverride;
    flow_deps.log = deps.log;
    flow_deps.warn = deps.warn;
    // 議論ごとのラウンド/ターン数 (discussion/improvement のみ。run_flow が config 既定にフォールバック)。
    flow_deps.rounds = input.rounds;
    flow_deps.turnsPerRound = input.turnsPerRound;
    flow_deps.personaIds = input.personaIds;
    flow_deps.state = deps.state;

    foresight::ForesightAfterFlowDeps foresight_deps;
    foresight_deps.llm = deps.llm.get();
    foresight_deps.onUtterance = deps.onUtterance;
    foresight_deps.listExternalVoices = list_external_voices;
    foresight_deps.log = deps.log;
    foresight_deps.warn = deps.warn;
    foresight_deps.sessionId = deps.sessionId;

    // 議論エンジン (respec PR-B): flow.engine="dialectic" で discussion/improvement を
    // 論証状態機械 (run_dialectic_flow) に分岐する。テスト用 drivers 差し替えは engine 設定より優先する。
    const std::string engine = config::get_config().flow.engine;
    persona_engine::LLMClient& llm = *deps.llm;

    DispatchResult out;
    out.kind = *kind;
    out.flow = *kind;

    // 代理議論 (観点タグ「代理」): 参加者を解決できたら engine 設定に依らず dialectic の代理モードで回す。
    // 起動できない理由は ProxyRequestError として呼び出し側へ返す (黙って通常議論にしない)。
    if (*kind == "discussion" || *kind == "improvement") {
        if (const auto proxy_run = proxy::resolve_proxy_run(theme, tags)) {
            FlowDirectorDeps proxy_deps = flow_deps;
            proxy_deps.proxyParticipants = proxy_run->participants;
            const FlowDriverFn run = *kind == "discussion"
                                         ? (deps.drivers.discussion ? deps.drivers.discussion : FlowDriverFn(dialectic::run_dialectic_discussion_flow))
                                         : (deps.drivers.improvement ? deps.drivers.improvement
                                                                     : FlowDriverFn(dialectic::run_dialectic_improvement_flow));
            out.result = foresight::run_with_foresight(
                proxy_run->theme, tags, *kind, [&] { return run(llm, proxy_run->theme, tags, proxy_deps); }, foresight_deps);
            return out;
        }
    }

    if (*kind == "discussion") {
        const FlowDriverFn run = deps.drivers.discussion ? deps.drivers.discussion
                                 : engine == "dialectic" ? FlowDriverFn(dialectic::run_dialectic_discussion_flow)
                                                         : FlowDriverFn(run_discussion_flow);
        out.result = foresight::run_with_foresight(theme, tags, *kind, [&] { return run(llm, theme, tags, flow_deps); }, foresight_deps);
        return out;
    }
    if (*kind == "improvement") {
        const FlowDriverFn run = deps.drivers.improvement ? deps.drivers.improvement
                                 : engine == "dialectic" ? FlowDriverFn(dialectic::run_dialectic_improvement_flow)
                                                         : default_improvement_driver();
        out.result = foresight::run_with_foresight(theme, tags, *kind, [&] { return run(llm, theme, tags, flow_deps); }, foresight_deps);
        return out;
    }
    if (*kind == "learning") {
        const LearningDriverFn run = deps.drivers.learning ? deps.drivers.learning : LearningDriverFn(run_learning_flow);
        if (!deps.core) throw std::runtime_error("learning フローには deps.core が必要です");
        LearningFlowDeps ld;
        ld.core = deps.core;
        ld.opinions = deps.opinions;
        ld.mechanics = deps.mechanics;
        ld.crawl = deps.learningCrawl;
        ld.sentimentClients = deps.sentimentClients;
        ld.slug = deps.slug;
        ld.workspaceId = deps.workspaceId;
        ld.gamesDir = deps.gamesDir;
        ld.log = deps.log;
        ld.warn = deps.warn;
        out.learning = run(theme, theme, ld);
        return out;
    }
    // sparring
    SparringDeps sd;
    sd.llm = deps.llm;
    sd.listExternalVoices = list_external_voices;
    sd.youtubeSearch = deps.youtubeSearch;
    sd.responsesPerTurn = deps.responsesPerTurn;
    sd.onUtterance = deps.onUtterance;
    sd.rng = deps.rng;
    sd.gamesDir = deps.gamesDir;
    sd.log = deps.log;
    sd.warn = deps.warn;
    sd.opponentPersonaIds = input.opponentPersonaIds ? *input.opponentPersonaIds : input.personaIds.value_or(std::vector<std::string>{});
    auto session = std::make_shared<SparringSession>(theme, tags, sd);
    session->start();
    out.session = std::move(session);
    return out;
}

} // namespace di::flow
