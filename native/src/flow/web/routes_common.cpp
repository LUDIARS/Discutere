#include "flow/web/routes_internal.hpp"

namespace di::flow::web {

namespace routes_detail {


// ─── 上限 ───────────────────────────────────────────────────────────────

JsonLimits body_limits(std::size_t max_bytes) { return JsonLimits{max_bytes, 200'000, 32}; }


State& state() {
    static State s;
    return s;
}


std::shared_ptr<const FlowWebDeps> current_deps() {
    std::lock_guard lock(state().m);
    return state().deps;
}


/** background_executor に積む (TS の void run)。満杯なら false。 */
bool submit_background(std::function<void()> fn) {
    auto& st = state();
    {
        std::lock_guard lock(st.m);
        ++st.inflight;
    }
    auto done = [] {
        auto& s = state();
        std::lock_guard lock(s.m);
        --s.inflight;
        s.idle_cv.notify_all();
    };
    const bool ok = background_executor().submit([fn = std::move(fn), done]() {
        try {
            fn();
        } catch (const std::exception& e) {
            log::warn(std::string("[flow-web] 背景の仕事が失敗: ") + e.what());
        } catch (...) {
            log::warn("[flow-web] 背景の仕事が失敗");
        }
        done();
    });
    if (!ok) done();
    return ok;
}


http::Response json_error(const std::string& message, int status) {
    return http::Response::json(Json{{"ok", false}, {"error", message}}, status);
}


http::Response not_initialized() { return json_error("flow web 未初期化", 500); }


/** c.req.json().catch(() => ({})) — 壊れた・上限超過・object 以外は {}。 */
Json read_body(const http::Request& req, std::size_t max_bytes) {
    auto j = req.json(body_limits(max_bytes));
    if (!j || !j->is_object()) return Json::object();
    return *j;
}


bool has_key(const Json& body, std::string_view key) { return body.is_object() && body.contains(std::string(key)); }


const Json* field(const Json& body, std::string_view key) {
    if (!body.is_object()) return nullptr;
    const auto it = body.find(std::string(key));
    return it == body.end() ? nullptr : &*it;
}


/** JS の !!obj[key] */
bool truthy_key(const Json& obj, std::string_view key) {
    const Json* v = field(obj, key);
    return v && json_truthy(*v);
}


/** typeof v === "string" ? v.trim() : "" */
std::string string_field(const Json& body, std::string_view key) {
    const Json* v = field(body, key);
    return v && v->is_string() ? str::trim(v->get<std::string>()) : "";
}


std::optional<std::string> raw_string(const Json& body, std::string_view key) {
    const Json* v = field(body, key);
    if (v && v->is_string()) return v->get<std::string>();
    return std::nullopt;
}


std::optional<std::string> model_or_null() {
    const auto& m = config::get_config().flow.paperReview.model;
    if (m.empty()) return std::nullopt;
    return m;
}



// ─── 入力の正規化 ────────────────────────────────────────────────────────

const std::vector<FlowTag>& valid_review_tags() {
    static const std::vector<FlowTag> v = {"機密", "内部", "運用", "開発", "未来予測", "代理"};
    return v;
}


std::vector<FlowTag> sanitize_review_tags(const Json* raw, const std::vector<FlowTag>& fallback) {
    std::vector<FlowTag> tags;
    auto add = [&](const std::string& t) {
        const auto& valid = valid_review_tags();
        if (std::find(valid.begin(), valid.end(), t) == valid.end()) return;
        if (std::find(tags.begin(), tags.end(), t) == tags.end()) tags.push_back(t);
    };
    if (raw && raw->is_array()) {
        for (const auto& t : *raw)
            if (t.is_string()) add(t.get<std::string>());
    } else {
        for (const auto& t : fallback) add(t);
    }
    return tags;
}


/** s.replace(/^(?:\d+[.)]\s+|[-*]\s+)/, "") */
std::string strip_list_marker(const std::string& s) {
    auto strip_spaces_after = [&](std::size_t p) -> std::optional<std::size_t> {
        std::size_t q = p;
        while (q < s.size()) {
            std::size_t next = q;
            if (!str::is_js_space(str::next_codepoint(s, next))) break;
            q = next;
        }
        if (q == p) return std::nullopt;  // \s+ は 1 文字以上
        return q;
    };
    std::size_t i = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
    if (i > 0 && i < s.size() && (s[i] == '.' || s[i] == ')')) {
        if (auto q = strip_spaces_after(i + 1)) return s.substr(*q);
    }
    if (!s.empty() && (s[0] == '-' || s[0] == '*')) {
        if (auto q = strip_spaces_after(1)) return s.substr(*q);
    }
    return s;
}


/** 論点フィールド (1 行 1 論点のテキスト or 配列) を issues[] に正規化する。 */
std::optional<std::vector<std::string>> parse_issues_field(const Json* raw) {
    std::vector<std::string> values;
    if (raw && raw->is_array()) {
        for (const auto& v : *raw)
            if (v.is_string()) values.push_back(v.get<std::string>());
    } else if (raw && raw->is_string()) {
        // split(/\r?\n/)
        const std::string s = raw->get<std::string>();
        std::size_t start = 0;
        for (std::size_t i = 0; i <= s.size(); ++i) {
            if (i == s.size() || s[i] == '\n') {
                std::size_t end = i;
                if (i < s.size() && end > start && s[end - 1] == '\r') --end;
                values.push_back(s.substr(start, end - start));
                start = i + 1;
            }
        }
    } else {
        return std::nullopt;
    }
    std::vector<std::string> out;
    for (const auto& v : values) {
        auto t = str::trim(strip_list_marker(v));
        if (!t.empty()) out.push_back(std::move(t));
    }
    return out;
}


std::optional<std::vector<std::string>> parse_persona_ids(const Json* raw) {
    std::vector<std::string> values;
    if (raw && raw->is_array()) {
        for (const auto& v : *raw)
            if (v.is_string()) values.push_back(v.get<std::string>());
    } else if (raw && raw->is_string()) {
        values = str::split(raw->get<std::string>(), ",");
    } else {
        return std::nullopt;
    }
    std::vector<std::string> ids;
    for (const auto& v : values) {
        auto t = str::trim(v);
        if (!t.empty()) ids.push_back(std::move(t));
    }
    if (ids.empty()) return std::nullopt;
    return ids;
}


/** readOptionalInt: number / 空でない文字列 → Number → 有限なら trunc して [min, max]。 */
std::optional<double> read_optional_int(const Json* raw, double min, double max) {
    std::optional<double> n;
    if (raw && raw->is_number()) n = raw->get<double>();
    else if (raw && raw->is_string() && !str::trim(raw->get<std::string>()).empty()) n = str::js_number(raw->get<std::string>());
    if (!n || !std::isfinite(*n)) return std::nullopt;
    return std::min(max, std::max(min, std::trunc(*n)));
}


/** 固定フォーム由来の seed (どれか 1 つでも値があれば)。 */
std::optional<PaperFixedFields> fixed_seed_from_body(const Json& body) {
    PaperFixedFields seed{string_field(body, "gameTitle"), string_field(body, "discussionTheme"), string_field(body, "discussionContent"),
                          string_field(body, "mechanicsContext"), string_field(body, "themeSupplement")};
    if (seed.gameTitle.empty() && seed.discussionTheme.empty() && seed.discussionContent.empty() && seed.mechanicsContext.empty() &&
        seed.themeSupplement.empty())
        return std::nullopt;
    return seed;
}


std::optional<PaperFixedFields> merge_mechanics_context(const std::optional<PaperFixedFields>& seed, const std::vector<std::optional<std::string>>& texts) {
    std::vector<std::string> values;
    std::vector<std::optional<std::string>> all;
    all.push_back(seed ? std::optional<std::string>(seed->mechanicsContext) : std::nullopt);
    for (const auto& t : texts) all.push_back(t);
    for (const auto& text : all) {
        if (!text) continue;
        auto t = str::trim(*text);
        if (!t.empty() && std::find(values.begin(), values.end(), t) == values.end()) values.push_back(std::move(t));
    }
    if (values.empty()) return seed;
    PaperFixedFields out = seed ? *seed : PaperFixedFields{};
    out.mechanicsContext = str::join(values, "\n\n");
    return out;
}


std::string flow_theme_from_seed(const std::optional<PaperFixedFields>& seed, const std::string& legacy_theme) {
    if (!seed) return legacy_theme;
    std::vector<std::string> parts;
    for (const std::string& v : {seed->gameTitle, seed->discussionTheme.empty() ? legacy_theme : seed->discussionTheme})
        if (!str::trim(v).empty()) parts.push_back(v);
    return str::join(parts, " / ");
}


PaperContent content_of(const PaperDraft& d) { return static_cast<const PaperContent&>(d); }


// ─── 仕様書 / Notion / GitHub / Anatomia ─────────────────────────────────

/** Notion リンクを探す入力欄 (仕様書 URL 欄 + 本文系の自由入力欄)。 */
const std::vector<std::string>& notion_source_fields() {
    static const std::vector<std::string> v = {"specUrl", "specText", "theme", "discussionTheme", "discussionContent", "mechanicsContext",
                                               "themeSupplement"};
    return v;
}


bool needs_github_token(const std::string& url) {
    const std::string l = str::to_lower_ascii(url);
    return str::contains(l, "github.com") || str::contains(l, "raw.githubusercontent.com");
}


std::string resolve_additional_spec_text(const Json& body, const FlowWebDeps& d) {
    std::vector<std::string> parts;
    auto warn = [](const std::string& m) { log::warn(m); };
    // Notion リンクはどの欄に貼られても Canalis でクロールする (素の fetch では本文が取れない)。
    std::vector<std::string> notion_parts;
    for (const auto& k : notion_source_fields())
        if (auto v = raw_string(body, k)) notion_parts.push_back(*v);
    ExpandNotionOpts notion_opts;
    notion_opts.crawler = d.notionCrawler;
    notion_opts.warn = [&](const std::string& m) { warn("[flow-web/notion] " + m); };
    const std::string notion_md = expand_notion_links(str::join(notion_parts, "\n"), notion_opts);
    if (!notion_md.empty()) parts.push_back(notion_md);

    const std::string spec_url = string_field(body, "specUrl");
    const std::string github_repo_url = string_field(body, "githubRepoUrl");
    const std::string github_path = string_field(body, "githubPath");
    const std::string github_ref = string_field(body, "githubRef");
    bool token_loaded = false;
    std::optional<std::string> github_token;
    auto token = [&]() {
        if (!token_loaded) {
            github_token = d.githubToken ? d.githubToken() : get_github_cli_token();
            token_loaded = true;
        }
        return github_token;
    };

    if (!spec_url.empty() && !is_notion_url(spec_url)) {
        try {
            ResolveSpecOpts opts;
            opts.allowLocalPath = true;  // Web は loopback 信頼 (TS と同じ)。パス検査・上限は spec_source が課す
            opts.fetchImpl = d.specFetch;
            opts.extractor = d.specExtractor;
            if (needs_github_token(spec_url)) opts.githubToken = token();
            parts.push_back(resolve_spec_text(spec_url, opts));
        } catch (const std::exception& e) {
            warn("[flow-web/spec] spec source read failed (" + spec_url + "): " + e.what());
        }
    }

    if (!github_repo_url.empty()) {
        try {
            const auto source = build_github_spec_source(github_repo_url, github_path, github_ref);
            if (!source) throw std::runtime_error("GitHub repository URL could not be parsed");
            if (!source->path || source->path->empty()) throw std::runtime_error("GitHub path is required");
            ResolveSpecOpts opts;
            opts.fetchImpl = d.specFetch;
            opts.extractor = d.specExtractor;
            opts.githubToken = token();
            parts.push_back(fetch_github_spec_text(*source, opts).text);
        } catch (const std::exception& e) {
            warn("[flow-web/github] GitHub file read failed (" + github_repo_url + " " + github_path + "): " + e.what());
        }
    }
    return str::join(parts, "\n\n");
}


/**
 * Anatomia 事前情報メカニクスを解決する (config 有効 + 対象指定時のみ)。
 * 取得失敗 (bin 不在 / CLI 非 0 / JSON 不正) は例外を伝播させる (fail-fast)。
 */
std::optional<std::vector<MechanicSummary>> resolve_anatomia_extra_mechanics(const std::string& theme, const std::string& project,
                                                                             const std::string& repo, const FlowWebDeps& d,
                                                                             const std::function<void(const std::string&)>& warn) {
    const auto source = anatomia::resolve_anatomia_source(project, repo);
    if (!source) return std::nullopt;
    const auto& cfg = config::get_config().flow.anatomia;
    if (!cfg.enabled) {
        warn("Anatomia 連携は無効 (flow.anatomia.enabled=false) のため指定を無視します");
        return std::nullopt;
    }
    anatomia::GetAnatomiaMechanicsDeps deps;
    deps.binPath = cfg.binPath;
    deps.autoDraft = cfg.autoDraft;
    deps.timeout = std::chrono::milliseconds(static_cast<long long>(cfg.timeoutMs));
    if (!cfg.refineModel.empty()) deps.refineModel = cfg.refineModel;
    deps.llm = d.llm.get();
    deps.theme = theme;
    deps.runner = d.anatomiaRunner;
    deps.warn = warn;
    auto mechanics = anatomia::get_anatomia_mechanics(*source, deps);
    warn("Anatomia 由来メカニクス " + std::to_string(mechanics.size()) + " 件を事前情報に追加");
    return mechanics;
}

} // namespace routes_detail
} // namespace di::flow::web
