// src/flow/web/flow-prep.ts の移植。
#include "flow/web/flow_prep.hpp"

#include <algorithm>
#include <cmath>

#include "base/log.hpp"
#include "base/strings.hpp"
#include "config.hpp"

namespace di::flow::web {

namespace {

int to_int(double v) { return std::isfinite(v) ? static_cast<int>(std::trunc(v)) : 0; }

/** typeof v === "string" ? v.trim() : "" */
std::string trimmed_string(const Json& body, std::string_view key) {
    if (!body.is_object()) return "";
    const auto it = body.find(std::string(key));
    if (it == body.end() || !it->is_string()) return "";
    return str::trim(it->get<std::string>());
}

const Json* field(const Json& body, std::string_view key) {
    if (!body.is_object()) return nullptr;
    const auto it = body.find(std::string(key));
    return it == body.end() ? nullptr : &*it;
}

/**
 * learningAppId: number ならその値、空でない文字列なら Number(s)。
 * C++ は整数の appId だけを持つ (NaN = nullopt、小数は切り捨て。port-notes の差)。
 */
std::optional<long long> app_id_of(const Json& body) {
    const Json* v = field(body, "learningAppId");
    if (!v) return std::nullopt;
    std::optional<double> n;
    if (v->is_number()) n = v->get<double>();
    else if (v->is_string() && !str::trim(v->get<std::string>()).empty()) n = str::js_number(v->get<std::string>());
    if (!n || !std::isfinite(*n) || std::fabs(*n) > 9.0e15) return std::nullopt;
    return static_cast<long long>(std::trunc(*n));
}

std::optional<std::string> subreddit_of(const Json& body) {
    const auto s = trimmed_string(body, "learningSubreddit");
    if (s.empty()) return std::nullopt;
    return s;
}

/** s.split(/[\s,]+/).filter(Boolean) */
std::vector<std::string> split_space_comma(std::string_view s) {
    std::vector<std::string> out;
    std::string cur;
    for (std::size_t i = 0; i < s.size();) {
        std::size_t next = i;
        const auto cp = str::next_codepoint(s, next);
        if (cp == ',' || str::is_js_space(cp)) {
            if (!cur.empty()) out.push_back(std::move(cur));
            cur.clear();
        } else {
            cur.append(s.substr(i, next - i));
        }
        i = next;
    }
    if (!cur.empty()) out.push_back(std::move(cur));
    return out;
}

/** typeof q === "string" ? q.trim() || undefined : undefined */
std::optional<std::string> nonempty_query(const Json& body) {
    const auto q = trimmed_string(body, "learningQuery");
    if (q.empty()) return std::nullopt;
    return q;
}

} // namespace

std::optional<std::string> resolve_youtube_api_key(const FlowWebDeps& d) {
    if (d.getYoutubeApiKey) return d.getYoutubeApiKey();
    return d.youtubeApiKey;
}

bool web_paper_gate_enabled() {
    const auto& pr = config::get_config().flow.paperReview;
    return pr.enabled || pr.webCanonical;
}

std::optional<AutoCrawlSpec> build_auto_crawl_spec(const Json& body) {
    const auto& cfg = config::get_config().flow.autoCrawl;
    if (!cfg.enabled) return std::nullopt;
    const std::string requested = trimmed_string(body, "learningSource");
    const std::string source = requested.empty() ? cfg.source : requested;
    if (!is_auto_crawl_source(source)) return std::nullopt;
    AutoCrawlSpec spec;
    spec.source = source;
    if (const Json* q = field(body, "learningQuery"); q && q->is_string()) spec.query = str::trim(q->get<std::string>());
    spec.appId = app_id_of(body);
    if (const Json* u = field(body, "learningUrls")) {
        if (u->is_array()) {
            std::vector<std::string> urls;
            for (const auto& x : *u)
                if (x.is_string() && !str::trim(x.get<std::string>()).empty()) urls.push_back(x.get<std::string>());
            spec.urls = std::move(urls);
        } else if (u->is_string() && !str::trim(u->get<std::string>()).empty()) {
            spec.urls = split_space_comma(u->get<std::string>());
        }
    }
    spec.subreddit = subreddit_of(body);
    return spec;
}

Json to_json(const AutoCrawlSpec& s) {
    Json j = Json::object();
    j["source"] = s.source;
    if (s.query) j["query"] = *s.query;
    if (s.appId) j["appId"] = *s.appId;
    if (s.urls) j["urls"] = *s.urls;
    if (s.subreddit) j["subreddit"] = *s.subreddit;
    return j;
}

std::optional<LearningCrawlSpec> build_learning_crawl(const Json& body, const std::optional<std::string>& youtube_api_key) {
    const auto& cfg = config::get_config().flow.autoCrawl;
    if (!cfg.enabled) return std::nullopt;
    const std::string requested = trimmed_string(body, "learningSource");
    const Json* balanced_flag = field(body, "learningBalanced");
    const bool balanced = requested == "balanced" || (balanced_flag && balanced_flag->is_boolean() && balanced_flag->get<bool>());

    if (balanced) {
        LearningCrawlSpec spec;
        spec.sources = {"steam", "youtube", "reddit", "niconico"};
        spec.query = nonempty_query(body);
        spec.maxItems = std::max(10, static_cast<int>(std::floor(cfg.maxItems / static_cast<double>(spec.sources.size()))));
        spec.youtubeApiKey = youtube_api_key;
        LearningSourceParams steam;
        steam.appId = app_id_of(body);
        LearningSourceParams reddit;
        reddit.subreddit = subreddit_of(body);
        spec.specBySource["steam"] = steam;
        spec.specBySource["reddit"] = reddit;
        return spec;
    }

    // UI 明示ソース: build_auto_crawl_spec の解決 (appId/urls 込み) をそのまま単一ソースに倒す。
    if (!requested.empty()) {
        const auto auto_spec = build_auto_crawl_spec(body);
        if (!auto_spec) return std::nullopt;
        LearningCrawlSpec spec;
        spec.sources = {auto_spec->source};
        spec.query = auto_spec->query;
        spec.maxItems = to_int(cfg.maxItems);
        spec.youtubeApiKey = youtube_api_key;
        LearningSourceParams p;
        p.appId = auto_spec->appId;
        p.urls = auto_spec->urls;
        p.subreddit = auto_spec->subreddit;
        spec.specBySource[auto_spec->source] = p;
        return spec;
    }

    // 既定: config のソースを自動経路に絞って横断収集。
    auto sources = resolve_auto_crawl_sources(cfg.sources, youtube_api_key);
    if (sources.empty()) return std::nullopt;
    LearningCrawlSpec spec;
    spec.sources = std::move(sources);
    spec.query = nonempty_query(body);
    spec.maxItems = to_int(cfg.maxItems);
    spec.youtubeApiKey = youtube_api_key;
    return spec;
}

Json to_json(const LearningCrawlSpec& s) {
    Json j = Json::object();
    j["sources"] = s.sources;
    if (s.query) j["query"] = *s.query;
    if (s.maxItems) j["maxItems"] = *s.maxItems;
    if (s.youtubeApiKey) j["youtubeApiKey"] = *s.youtubeApiKey;
    if (!s.specBySource.empty()) {
        // TS のオブジェクトリテラルの順 (= sources の順) で書く。
        Json by = Json::object();
        for (const auto& src : s.sources) {
            const auto it = s.specBySource.find(src);
            if (it == s.specBySource.end()) continue;
            Json p = Json::object();
            if (it->second.appId) p["appId"] = *it->second.appId;
            if (it->second.urls) p["urls"] = *it->second.urls;
            if (it->second.subreddit) p["subreddit"] = *it->second.subreddit;
            by[src] = p;
        }
        j["specBySource"] = by;
    }
    return j;
}

void prepare_information_before_flow(const FlowKind& kind, const std::string& theme, const std::vector<FlowTag>& tags,
                                     const std::string& session_id, const std::optional<AutoCrawlSpec>& spec, const FlowWebDeps& d) {
    // 機密/内部は外部収集をしない (情報ゲートの不足観点クロールも旧 autoCrawl も省く)。
    if (!can_collect_external(tags)) {
        log::info("[flow-gate] 機密/内部タグのため外部の声の収集を省略");
        return;
    }
    // 情報ゲート優先 (LLM 評価 + 不足観点クロール)。
    try {
        FlowGateArgs args;
        args.kind = kind;
        args.theme = theme;
        args.tags = tags;
        args.llm = d.llm.get();
        args.openCore = d.openCore;
        args.workspaceId = d.workspaceId;
        args.listExternalVoices = d.listExternalVoices;
        args.sessionId = session_id;
        args.log = [](const std::string& m) { log::info("[flow-gate] " + m); };
        args.warn = [](const std::string& m) { log::warn("[flow-gate] " + m); };
        args.youtubeApiKey = resolve_youtube_api_key(d);
        args.collectAndImportFn = d.gateCollectAndImport;
        args.evaluateFn = d.gateEvaluate;
        if (gate_before_flow(args)) return;  // ゲートが学習まで担った
    } catch (const std::exception& e) {
        log::warn(std::string("[flow-gate] 情報ゲート失敗 (議論は続行): ") + e.what());
    }
    // フォールバック: 従来のカウント閾値 autoCrawl。
    legacy_auto_crawl_before_flow(kind, theme, spec, d);
}

void legacy_auto_crawl_before_flow(const FlowKind& kind, const std::string& theme, const std::optional<AutoCrawlSpec>& spec,
                                   const FlowWebDeps& d) {
    if (!spec || !d.openCore || (kind != "discussion" && kind != "improvement")) return;
    const auto& cfg = config::get_config().flow.autoCrawl;
    std::unique_ptr<core::Core> core;
    try {
        core = d.openCore();
        EnsureLearningDataDeps deps;
        deps.core = core.get();
        deps.theme = theme;
        deps.slug = derive_slug(theme);
        deps.workspaceId = d.workspaceId;
        deps.spec = *spec;
        deps.minVoices = to_int(cfg.minVoices);
        deps.maxItems = to_int(cfg.maxItems);
        deps.listExternalVoices = d.listExternalVoices;
        deps.youtubeApiKey = resolve_youtube_api_key(d);
        deps.collectors = d.collectors;
        deps.importItems = d.importItems;
        deps.log = [](const std::string& m) { log::info("[flow-autocrawl] " + m); };
        deps.warn = [](const std::string& m) { log::warn("[flow-autocrawl] " + m); };
        const auto result = ensure_learning_data(deps);
        if (!result.skipped) log::info("[flow-autocrawl] " + result.message);
    } catch (const std::exception& e) {
        log::warn(std::string("[flow-autocrawl] クロール失敗 (議論は続行): ") + e.what());
    }
    if (core) core->close();
}

} // namespace di::flow::web
