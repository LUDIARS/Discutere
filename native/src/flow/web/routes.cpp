// src/flow/web/routes.ts の移植。経路・JSON の形・status は TS と同じ。
#include "flow/web/routes.hpp"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>

#include "api/js_query.hpp"
#include "base/crypto.hpp"
#include "base/executor.hpp"
#include "base/log.hpp"
#include "base/strings.hpp"
#include "base/time.hpp"
#include "config.hpp"
#include "flow/anatomia/index.hpp"
#include "flow/db/connection.hpp"
#include "flow/debatability.hpp"
#include "flow/discussion_paper.hpp"
#include "flow/foresight/state.hpp"
#include "flow/games_md.hpp"
#include "flow/github_cli.hpp"
#include "flow/information_gate_runner.hpp"
#include "flow/mechanic_extract.hpp"
#include "flow/paper_blocks.hpp"
#include "flow/paper_markdown.hpp"
#include "flow/paper_reuse.hpp"
#include "flow/paper_review.hpp"
#include "flow/paper_revisions.hpp"
#include "flow/paper_voices.hpp"
#include "flow/persona_display.hpp"
#include "flow/project_premise.hpp"
#include "flow/spec_analyze.hpp"
#include "flow/web/page.hpp"

namespace di::flow::web {

namespace {

// ─── 上限 ───────────────────────────────────────────────────────────────

JsonLimits body_limits(std::size_t max_bytes) { return JsonLimits{max_bytes, 200'000, 32}; }
/** LLM の応答 (チェック系の JSON object) を読む上限。 */
const JsonLimits kLlmJsonLimits{1u << 20, 50'000, 32};
/** DB の JSON 列 (top_utterance_ids_json / aufhebung_json) を読む上限。 */
const JsonLimits kDbJsonLimits{1u << 20, 50'000, 16};
/** status が読む行の上限。 */
const sql::QueryLimits kStatusRowLimits{200'000, 256u << 20};

// ─── モジュールの状態 (TS の module-level Map / Set) ──────────────────────

/**
 * ペーパーレビュー待ち (sessionId → 草案 + 起動入力)。
 * /start で草案構築をバックグラウンド起動し、ready になったらブラウザが /paper で取得して
 * 直接編集 → /paper/approve で議論を開始する。フィールドは State::m で守る。
 */
struct WebPaperReview {
    FlowKind flow;
    std::optional<double> rounds;
    std::optional<double> turnsPerRound;
    std::optional<std::vector<std::string>> personaIds;
    /** 草案構築が完了したか (false の間はブラウザは待つ)。 */
    bool ready = false;
    /** 構築失敗時のメッセージ (あれば)。 */
    std::optional<std::string> error;
    std::optional<PaperDraft> draft;
    /** 集めた情報のサマリ (PaperReviewInfo の JSON。TS と同じく緩い object として持つ)。 */
    std::optional<Json> info;
    /** 無操作の自動開始タイマー (timeoutMs>0 時のみ。調整があるたび張り直す)。 */
    std::optional<Scheduler::Id> timer;
};

struct State {
    std::mutex m;
    std::shared_ptr<const FlowWebDeps> deps;
    /** flow=壁打ち の起動済みセッション (HTTP の後続発話を受けるため)。 */
    std::map<std::string, std::shared_ptr<SparringSession>> sparring;
    /** discussion/improvement の完了状態 (status の done 判定用)。 */
    std::set<std::string> finished;
    std::map<std::string, std::shared_ptr<WebPaperReview>> reviews;
    /** ペーパーを使い回したセッションの声の検索 (承認で議論に渡して外す)。 */
    std::map<std::string, VoiceLookup> voiceLookups;
    /** 進行中の議論の進行状態 (DELETE で止める)。 */
    std::map<std::string, std::shared_ptr<FlowRunState>> running;
    // 背景の仕事の数 (テストが終わりを待つため)
    std::condition_variable idle_cv;
    int inflight = 0;
};

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

const char* kBusyMessage = "サーバが混み合っています。しばらくしてから再度お試しください";

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

constexpr double kMaxSafeInteger = 9007199254740991.0;

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

// ─── 合成の声 (学習フロー) ────────────────────────────────────────────────

struct Range {
    std::uint32_t first;
    std::uint32_t last;
};
constexpr Range kLetterNumber[] = {
#include "flow/unicode_letter_number.inc"
};

bool is_letter_or_number(std::uint32_t cp) {
    const auto* end = std::end(kLetterNumber);
    const auto* it = std::upper_bound(std::begin(kLetterNumber), end, cp, [](std::uint32_t v, const Range& r) { return v < r.first; });
    if (it == std::begin(kLetterNumber)) return false;
    --it;
    return cp >= it->first && cp <= it->last;
}

bool ascii_ci_equal_at(std::string_view s, std::size_t pos, std::string_view lower_kw) {
    if (pos + lower_kw.size() > s.size()) return false;
    for (std::size_t i = 0; i < lower_kw.size(); ++i) {
        char c = s[pos + i];
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (c != lower_kw[i]) return false;
    }
    return true;
}

std::size_t skip_js_spaces(std::string_view s, std::size_t p) {
    while (p < s.size()) {
        std::size_t next = p;
        if (!str::is_js_space(str::next_codepoint(s, next))) break;
        p = next;
    }
    return p;
}

std::string normalize_synthetic_voice_text(const std::string& raw) {
    std::string s = raw;
    // /^\s*\[synthetic\]\s*/i
    {
        const std::size_t p = skip_js_spaces(s, 0);
        if (ascii_ci_equal_at(s, p, "[synthetic]")) s = s.substr(skip_js_spaces(s, p + 11));
    }
    // /^\s*\[[^\]]*synthetic[^\]]*\]\s*/i
    {
        const std::size_t p = skip_js_spaces(s, 0);
        if (p < s.size() && s[p] == '[') {
            const std::size_t close = s.find(']', p + 1);
            if (close != std::string::npos) {
                const std::string inner = str::to_lower_ascii(std::string_view(s).substr(p + 1, close - p - 1));
                if (str::contains(inner, "synthetic")) s = s.substr(skip_js_spaces(s, close + 1));
            }
        }
    }
    return str::js_slice(str::trim(str::collapse_whitespace(s)), 0, 500);
}

/** key を UTF-16 のコード単位列にする (JS の文字列の length / slice と同じ数え方)。 */
std::u16string to_utf16(std::string_view s) {
    std::u16string out;
    for (std::size_t i = 0; i < s.size();) {
        std::uint32_t cp = str::next_codepoint(s, i);
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back(static_cast<char16_t>(cp));
        }
    }
    return out;
}

std::string synthetic_similarity_key(const std::string& text) {
    const std::string lower = str::replace_all(str::to_lower(text), "[synthetic]", "");
    std::string out;
    for (std::size_t i = 0; i < lower.size();) {
        const std::size_t start = i;
        const auto cp = str::next_codepoint(lower, i);
        if (is_letter_or_number(cp)) out.append(lower, start, i - start);
    }
    return out;
}

std::set<std::uint64_t> synthetic_shingles(const std::u16string& key) {
    std::set<std::uint64_t> out;
    if (key.size() <= 2) {
        if (key.empty()) return out;
        const std::uint64_t a = key[0];
        const std::uint64_t b = key.size() > 1 ? key[1] : 0;
        out.insert((static_cast<std::uint64_t>(key.size()) << 32) | (a << 16) | b);
        return out;
    }
    for (std::size_t i = 0; i + 1 < key.size(); ++i)
        out.insert((2ull << 32) | (static_cast<std::uint64_t>(key[i]) << 16) | static_cast<std::uint64_t>(key[i + 1]));
    return out;
}

double synthetic_voice_similarity(const std::string& a, const std::string& b) {
    const std::string ak = synthetic_similarity_key(a);
    const std::string bk = synthetic_similarity_key(b);
    if (ak.empty() || bk.empty()) return 0;
    const auto a16 = to_utf16(ak);
    const auto b16 = to_utf16(bk);
    const bool a_short = a16.size() <= b16.size();
    const std::string& short_s = a_short ? ak : bk;
    const std::string& long_s = a_short ? bk : ak;
    if ((a_short ? a16.size() : b16.size()) >= 18 && str::contains(long_s, short_s)) return 1;
    const auto as = synthetic_shingles(a16);
    const auto bs = synthetic_shingles(b16);
    std::size_t overlap = 0;
    for (const auto s : as)
        if (bs.count(s)) ++overlap;
    const std::size_t uni = as.size() + bs.size() - overlap;
    return uni > 0 ? static_cast<double>(overlap) / static_cast<double>(uni) : 0;
}

std::string compact_synthetic_label(const Json* raw, const std::string& fallback) {
    if (!raw || !raw->is_string()) return fallback;
    const std::string text = str::js_slice(str::collapse_whitespace(str::trim(raw->get<std::string>())), 0, 32);
    return text.empty() ? fallback : text;
}

/** a ?? b ?? c (null / 無いキーを飛ばす)。 */
const Json* first_present(const Json* obj, std::initializer_list<const char*> keys) {
    if (!obj) return nullptr;
    for (const char* k : keys) {
        const auto it = obj->find(k);
        if (it != obj->end() && !it->is_null()) return &*it;
    }
    return nullptr;
}

std::string coerce_synthetic_polarity(const Json* raw) {
    if (!raw || !raw->is_string()) return "mixed";
    const std::string text = str::to_lower(str::trim(raw->get<std::string>()));
    if (text == "positive" || str::contains(text, "肯定") || str::contains(text, "賛成")) return "positive";
    if (text == "negative" || str::contains(text, "否定") || str::contains(text, "反対") || str::contains(text, "不満")) return "negative";
    return "mixed";
}

std::string polarity_label(const std::string& p) {
    if (p == "positive") return "肯定";
    if (p == "negative") return "否定";
    return "混合";
}

struct SyntheticCandidate {
    std::string content;
    std::string segment;
    std::string concern;
    std::string polarity;
    std::string similarityText;
};

std::optional<SyntheticCandidate> coerce_synthetic_candidate(const Json& raw) {
    const Json* obj = raw.is_object() ? &raw : nullptr;
    std::string text;
    if (raw.is_string()) text = raw.get<std::string>();
    else if (obj && obj->contains("content") && (*obj)["content"].is_string()) text = (*obj)["content"].get<std::string>();
    const std::string content = normalize_synthetic_voice_text(text);
    if (content.empty()) return std::nullopt;
    SyntheticCandidate c;
    c.segment = compact_synthetic_label(first_present(obj, {"segment", "persona", "userSegment"}), "仮想ユーザ");
    c.concern = compact_synthetic_label(first_present(obj, {"concern", "issue", "topic"}), "未分類の論点");
    c.polarity = coerce_synthetic_polarity(first_present(obj, {"polarity", "sentiment"}));
    c.similarityText = c.segment + " " + c.concern + " " + content;
    c.content = "[synthetic] (" + c.segment + " / " + c.concern + " / " + polarity_label(c.polarity) + ") " + content;
    return c;
}

std::vector<SyntheticCandidate> select_diverse_synthetic(const Json& raw_items, std::size_t limit = 8) {
    std::vector<SyntheticCandidate> candidates;
    if (raw_items.is_array()) {
        // LLM の応答由来なので候補数に上限を課す (TS は全件)。
        for (const auto& item : raw_items) {
            if (candidates.size() >= 200) break;
            if (auto c = coerce_synthetic_candidate(item)) candidates.push_back(std::move(*c));
        }
    }
    std::vector<SyntheticCandidate> selected;
    std::set<std::size_t> selected_indexes;
    std::set<std::string> used_slots;
    const std::vector<std::string> order = {"negative", "mixed", "positive", "negative", "mixed", "positive", "negative", "mixed"};
    auto try_pick = [&](const std::string* wanted) {
        for (std::size_t i = 0; i < candidates.size(); ++i) {
            if (selected_indexes.count(i)) continue;
            const auto& c = candidates[i];
            if (wanted && c.polarity != *wanted) continue;
            const std::string slot = str::to_lower(c.segment) + "|" + str::to_lower(c.concern);
            if (used_slots.count(slot)) continue;
            bool similar = false;
            for (const auto& s : selected)
                if (synthetic_voice_similarity(s.similarityText, c.similarityText) >= 0.58) {
                    similar = true;
                    break;
                }
            if (similar) continue;
            selected.push_back(c);
            selected_indexes.insert(i);
            used_slots.insert(slot);
            return true;
        }
        return false;
    };
    for (const auto& p : order) {
        if (selected.size() >= limit) break;
        try_pick(&p);
    }
    while (selected.size() < limit && try_pick(nullptr)) {
        // 重ならない残りの候補で埋める。
    }
    return selected;
}

std::vector<LearningOpinion> synthesize_learning_opinions(const std::string& theme, const std::vector<GameMechanicEntry>& mechanics,
                                                          const std::string& mechanics_context, persona_engine::LLMClient& llm,
                                                          const std::function<void(const std::string&)>& warn) {
    std::string mechanics_text;
    if (!mechanics.empty()) {
        std::vector<std::string> lines;
        for (std::size_t i = 0; i < mechanics.size() && i < 20; ++i)
            lines.push_back(std::to_string(i + 1) + ". " + mechanics[i].name + ": " + mechanics[i].description.value_or(""));
        mechanics_text = str::join(lines, "\n");
    } else {
        mechanics_text = str::js_slice(mechanics_context, 0, 6000);
        if (mechanics_text.empty()) mechanics_text = theme;
    }
    const std::string system =
        "You create plausible hypothesis-labeled player feedback for game design learning. "
        "Return JSON only. Do not include real personal data or claim these are real reviews.";
    const std::string prompt =
        "Game/theme:\n" + theme + "\n\nMechanics/design notes:\n" + mechanics_text + "\n\n" +
        "Create 12 candidate user voice snippets so the application can keep the most diverse 8. "
        "Use Japanese unless the input is not Japanese. Cover distinct player segments such as first-time player, returning player, experienced player, no-pay/casual player, co-op helper, and monetization-sensitive player. "
        "Cover distinct concerns such as tutorial clarity, early difficulty, reward pacing, onboarding UI, character growth/gacha, social/co-op, and re-engagement. "
        "Balance polarity with at least 4 negative, 3 mixed, and 3 positive candidates. "
        "Do not repeat the same segment+concern pair, conclusion, or sentence frame. "
        "Each item must be {\"segment\":\"...\",\"polarity\":\"positive|negative|mixed\",\"concern\":\"...\",\"content\":\"...\"} and content must start with \"[synthetic] \".";
    try {
        persona_engine::LLMInvokeArgs args;
        args.system = system;
        args.prompt = prompt;
        args.maxTokens = 2500;
        const auto res = llm.invoke(args);
        if (!res.ok) {
            warn("synthetic user voice generation failed: " + res.error);
            return {};
        }
        const auto arr = extract_json_array(res.text);
        if (!arr) return {};
        auto opinions = normalize_synthetic_learning_opinions(*arr, static_cast<double>(time::now_ms()));
        if (opinions.size() < 5) warn("synthetic user voice generation produced only " + std::to_string(opinions.size()) + " diverse items");
        return opinions;
    } catch (const std::exception& e) {
        warn(std::string("synthetic user voice generation threw: ") + e.what());
        return {};
    }
}

// ─── ペーパーのチェック系 ────────────────────────────────────────────────

/** defaultPaperInfo(existing) */
Json default_paper_info(const std::optional<Json>& existing) {
    if (existing) return *existing;
    return Json{{"voiceCount", 0}, {"countCapped", false}, {"samples", Json::array()}};
}

std::optional<Json> stored_paper_review_info(const std::string& session_id) {
    try {
        auto info = get_paper_review_info(session_id);
        if (info && info->is_object()) return info;
    } catch (const std::exception& e) {
        log::warn(std::string("[flow-web/paper] review info 読み込み失敗: ") + e.what());
    }
    return std::nullopt;
}

void save_paper_review_info(const std::string& session_id, const Json& info) {
    try {
        set_paper_review_info(session_id, info);
    } catch (const std::exception& e) {
        log::warn("[flow-web/paper " + session_id + "] review info 保存失敗: " + e.what());
    }
}

constexpr int REVIEW_INFO_SAMPLE_LIMIT = 9;
constexpr int DEBATABILITY_VOICE_LIMIT = 50;

Json review_info_samples(const std::vector<ContextVoice>& voices) {
    Json out = Json::array();
    for (std::size_t i = 0; i < voices.size() && i < static_cast<std::size_t>(REVIEW_INFO_SAMPLE_LIMIT); ++i) {
        const auto& v = voices[i];
        const std::string content = str::js_length(v.content) > 120 ? str::js_slice(v.content, 0, 120) + "..." : v.content;
        out.push_back(Json{{"content", content}, {"source", v.source}});
    }
    return out;
}

std::optional<Json> extract_json_object(const std::string& raw) {
    const auto start = raw.find('{');
    const auto end = raw.rfind('}');
    if (start == std::string::npos || end == std::string::npos || end <= start) return std::nullopt;
    auto parsed = parse_json(std::string_view(raw).substr(start, end - start + 1), kLlmJsonLimits);
    if (!parsed || !parsed->is_object()) return std::nullopt;
    return parsed;
}

std::string short_string(const Json* raw, const std::string& fallback = "") {
    if (!raw || !raw->is_string()) return fallback;
    return str::js_slice(str::trim(raw->get<std::string>()), 0, 2000);
}

Json string_array(const Json* raw, std::size_t limit) {
    Json out = Json::array();
    if (!raw || !raw->is_array()) return out;
    for (const auto& v : *raw) {
        if (out.size() >= limit) break;
        if (!v.is_string() || str::trim(v.get<std::string>()).empty()) continue;
        out.push_back(str::js_slice(str::trim(v.get<std::string>()), 0, 1000));
    }
    return out;
}

std::string paper_md_with_premise(const std::optional<std::string>& flow, const std::string& body_md) {
    std::vector<std::string> parts;
    const std::string premise = project_premise(flow);
    if (!premise.empty()) parts.push_back(premise);
    if (!body_md.empty()) parts.push_back(body_md);
    return str::join(parts, "\n\n");
}

Json suggest_fixes_for_paper(const std::optional<std::string>& flow, const PaperDraft& draft, const Json& info, persona_engine::LLMClient& llm,
                             const std::function<void(const std::string&)>& warn) {
    const Json* understanding = field(info, "understanding");
    const Json* debatability = field(info, "debatability");
    const bool understanding_ng = understanding && understanding->is_object() && understanding->contains("ok") &&
                                  (*understanding)["ok"].is_boolean() && !(*understanding)["ok"].get<bool>();
    const bool debatability_ng = debatability && json_truthy(*debatability) && !truthy_key(*debatability, "degraded") &&
                                 !truthy_key(*debatability, "debatable");
    if (!understanding_ng && !debatability_ng) return Json::array();
    const std::string system =
        "You review a Japanese discussion paper before debate. Return JSON only. "
        "Create concrete fix suggestions from the provided check findings. Do not rewrite the whole paper. "
        "Make suggestedChange directly appendable. Use {{placeholder}} only when the editor must provide missing specifics.";
    Json prompt_obj = Json::object();
    prompt_obj["paperMd"] = paper_md_with_premise(flow, draft.bodyMd);
    prompt_obj["understanding"] = understanding ? *understanding : Json(nullptr);
    prompt_obj["debatability"] = debatability ? *debatability : Json(nullptr);
    prompt_obj["expectedShape"] = Json::array({Json{{"title", "短い見出し"},
                                                     {"reason", "指摘理由"},
                                                     {"suggestedChange", "そのまま追記できる修正文。必要なら {{具体例}} のようなプレースホルダを含める。"}}});
    try {
        persona_engine::LLMInvokeArgs args;
        args.system = system;
        args.prompt = json_stringify_pretty(prompt_obj, 2);
        args.model = model_or_null();
        args.maxTokens = 1800;
        const auto res = llm.invoke(args);
        if (!res.ok) {
            warn("paper-fix-suggestions LLM error: " + res.error);
            return Json::array();
        }
        const auto arr = extract_json_array(res.text);
        if (!arr || !arr->is_array()) return Json::array();
        Json out = Json::array();
        for (const auto& raw : *arr) {
            if (out.size() >= 5) break;
            if (!raw.is_object()) continue;
            const std::string title = short_string(field(raw, "title"));
            const std::string reason = short_string(field(raw, "reason"));
            const std::string change = short_string(field(raw, "suggestedChange"));
            if (title.empty() || change.empty()) continue;
            out.push_back(Json{{"title", title}, {"reason", reason}, {"suggestedChange", change}});
        }
        return out;
    } catch (const std::exception& e) {
        warn(std::string("paper-fix-suggestions threw: ") + e.what());
        return Json::array();
    }
}

Json check_mechanics_knowledge(const PaperDraft& draft, const PaperFixedFields& fields, persona_engine::LLMClient& llm,
                               const std::function<void(const std::string&)>& warn) {
    const std::string system =
        "You check whether you have enough game mechanics knowledge to support a debate. "
        "Return JSON only. Do not invent certainty; mark low confidence when details are missing.";
    Json mechanics = Json::array();
    for (std::size_t i = 0; i < draft.mechanics.size() && i < 30; ++i) mechanics.push_back(to_json(draft.mechanics[i]));
    Json prompt_obj = Json::object();
    prompt_obj["gameTitle"] = fields.gameTitle;
    prompt_obj["discussionTheme"] = fields.discussionTheme.empty() ? draft.theme : fields.discussionTheme;
    prompt_obj["providedMechanicsContext"] = fields.mechanicsContext;
    prompt_obj["extractedMechanics"] = mechanics;
    prompt_obj["expectedShape"] = Json{{"ok", true},
                                       {"confidence", "low|medium|high"},
                                       {"summary", "理解状況の短い説明"},
                                       {"knownMechanics", Json::array({"把握しているメカニクス"})},
                                       {"missingQuestions", Json::array({"不足している確認事項"})}};
    try {
        persona_engine::LLMInvokeArgs args;
        args.system = system;
        args.prompt = json_stringify_pretty(prompt_obj, 2);
        args.model = model_or_null();
        args.maxTokens = 1800;
        const auto res = llm.invoke(args);
        if (!res.ok) throw std::runtime_error(res.error);
        const auto obj = extract_json_object(res.text);
        if (!obj) throw std::runtime_error("no JSON object");
        const Json* conf = field(*obj, "confidence");
        std::string confidence = "low";
        if (conf && conf->is_string() && (conf->get<std::string>() == "high" || conf->get<std::string>() == "medium"))
            confidence = conf->get<std::string>();
        const Json known = string_array(field(*obj, "knownMechanics"), 12);
        const Json missing = string_array(field(*obj, "missingQuestions"), 8);
        const Json* okv = field(*obj, "ok");
        const bool ok = okv && okv->is_boolean() ? okv->get<bool>() : (confidence != "low" && missing.empty());
        Json out = Json::object();
        out["ok"] = ok;
        out["confidence"] = confidence;
        out["summary"] = short_string(field(*obj, "summary"), "メカニクス理解度を確認しました。");
        out["knownMechanics"] = known;
        out["missingQuestions"] = missing;
        return out;
    } catch (const std::exception& e) {
        warn(std::string("mechanics-knowledge check failed: ") + e.what());
        Json out = Json::object();
        out["ok"] = false;
        out["confidence"] = "low";
        out["summary"] = "メカニクス知識確認に失敗しました。仕様書・ゲーム内容・主要ループを補足してください。";
        out["knownMechanics"] = Json::array();
        out["missingQuestions"] = Json::array({"ゲームの基本ループ、主要システム、報酬、制約を補足してください。"});
        return out;
    }
}

Json voice_sim(bool possible, const char* confidence, const char* summary, const char* next_action) {
    Json j = Json::object();
    j["possible"] = possible;
    j["confidence"] = confidence;
    j["summary"] = summary;
    j["caveat"] = "LLM生成のユーザの声は実ユーザの代替ではなく、仮説として扱ってください。";
    j["nextAction"] = next_action;
    return j;
}

Json assess_voice_simulation_readiness(const PaperDraft& draft, const Json& info, const std::optional<std::string>& flow) {
    const auto fields = paper_fixed_fields_from_markdown(draft.bodyMd, content_of(draft));
    const Json* vc = field(info, "voiceCount");
    const double voice_count = vc && vc->is_number() ? vc->get<double>() : 0;
    const bool has_theme = (!requires_project_title(flow) || !str::trim(fields.gameTitle).empty()) && !str::trim(fields.discussionTheme).empty();
    const std::string mechanics_text = str::trim(fields.mechanicsContext);
    const bool has_mechanics_context = flow && *flow == "discussion"
                                           ? !str::trim(fields.discussionContent).empty()
                                           : (str::js_length(mechanics_text) >= 80 || !draft.mechanics.empty());
    const Json* mk = field(info, "mechanicsKnowledge");
    const bool mechanics_low = mk && mk->is_object() && mk->contains("ok") && (*mk)["ok"].is_boolean() && !(*mk)["ok"].get<bool>() &&
                               json_string(*mk, "confidence") == "low";
    if (!has_theme)
        return voice_sim(false, "low", "ゲームタイトルと議論テーマが不足しているため、ユーザの声の生成シミュレーションはまだ不安定です。",
                         "先にペーパーの基本項目を埋めてから、議論可能性を確認してください。");
    if (mechanics_low || !has_mechanics_context)
        return voice_sim(false, "low", "メカニクス理解が不足しているため、ユーザの声を生成しても根拠の薄い反応になりやすい状態です。",
                         "参考情報の取得またはメカニクス知識確認で、基本ループ・報酬・制約を補ってください。");
    if (voice_count >= 6)
        return voice_sim(true, "high", "外部のユーザの声を種に、LLMで不足セグメントの反応を補助シミュレーションできます。",
                         "生成した声は仮説ラベルを付け、実ユーザの声と分けて議論材料にしてください。");
    if (voice_count > 0)
        return voice_sim(true, "medium", "少数のユーザの声とメカニクス文脈を種に、LLMで仮想ユーザ反応を試作できます。",
                         "不足しているユーザ層を指定して生成し、可能なら参考情報の取得で検証してください。");
    return voice_sim(true, "low", "実ユーザの声は不足していますが、メカニクス文脈をもとに粗い仮想ユーザ反応は生成できます。",
                     "議論開始前に、生成結果を仮説として扱うか、参考情報の取得で実ユーザの声を補ってください。");
}

/** 議論可能性チェックの LLM (config で無効でも手動チェックでは走らせる)。 */
struct ManualGate {
    std::shared_ptr<persona_engine::LLMClient> owned;
    persona_engine::LLMClient* llm = nullptr;
    int minArmableIssues = 2;
};

std::optional<ManualGate> manual_debatability_gate(const FlowKind& kind, const std::string& session_id, persona_engine::LLMClient& llm) {
    if (kind != "discussion" && kind != "improvement") return std::nullopt;
    ManualGate g;
    if (auto o = resolve_debatability_gate(kind, session_id, llm)) {
        g.owned = o->llm;
        g.llm = o->llm.get();
        g.minArmableIssues = o->minArmableIssues;
    } else {
        g.llm = &llm;
        g.minArmableIssues = static_cast<int>(config::get_config().flow.debatability.minArmableIssues);
    }
    return g;
}

// ─── ペーパー編集の状態操作 (State::m を持って呼ぶ) ───────────────────────

std::int64_t latest_revision_rev(const std::string& session_id) {
    try {
        const auto rows = list_revisions(session_id);
        return rows.empty() ? 0 : rows.back().rev;
    } catch (const std::exception&) {
        return 0;
    }
}

/** ドラフト + ブロック分解 + 版履歴状態を 1 つの JSON にまとめる (Notion 風 UI 用)。 */
Json paper_payload_locked(const std::string& session_id, const PaperDraft& draft, const WebPaperReview* entry) {
    std::optional<Json> info = entry && entry->info ? entry->info : stored_paper_review_info(session_id);
    Json display_info = nullptr;
    if (info) {
        display_info = *info;
        const Json* vs = field(*info, "voiceSimulation");
        display_info["voiceSimulation"] =
            vs && !vs->is_null() ? *vs : assess_voice_simulation_readiness(draft, *info, entry ? std::optional<std::string>(entry->flow) : std::nullopt);
    }
    Json j = Json::object();
    const auto no = get_discussion_number_by_session(session_id);
    j["discussionNo"] = no ? Json(*no) : Json(nullptr);
    if (entry) j["flow"] = entry->flow;
    j["paper"] = to_json(draft);
    j["info"] = display_info;
    j["fixedFields"] = to_json(paper_fixed_fields_from_markdown(draft.bodyMd, content_of(draft)));
    j["settings"] = Json{{"rounds", entry && entry->rounds ? Json(*entry->rounds) : Json(nullptr)},
                         {"turnsPerRound", entry && entry->turnsPerRound ? Json(*entry->turnsPerRound) : Json(nullptr)}};
    Json blocks = Json::array();
    for (const auto& b : split_blocks(draft.bodyMd)) blocks.push_back(to_json(b));
    j["blocks"] = blocks;
    j["canRevert"] = can_revert(session_id);
    j["rev"] = latest_revision_rev(session_id);
    return j;
}

Json ok_with(const Json& payload, std::initializer_list<std::pair<const char*, Json>> before = {}) {
    Json out = Json::object();
    out["ok"] = true;
    for (const auto& [k, v] : before) out[k] = v;
    for (auto it = payload.begin(); it != payload.end(); ++it) out[it.key()] = it.value();
    return out;
}

NewPaper new_paper_of(const std::string& session_id, const PaperDraft& d) {
    NewPaper p;
    p.sessionId = session_id;
    p.theme = d.theme;
    p.tags = d.tags;
    p.mechanics = d.mechanics;
    p.supplement = d.supplement;
    p.bodyMd = d.bodyMd;
    return p;
}

void schedule_web_auto_approve_locked(const std::string& session_id);

/** ドラフトを本文 md で更新し、版履歴に追記して自動開始タイマーを延長する。 */
void commit_body_md_locked(const std::string& session_id, WebPaperReview& entry, const std::string& body_md, const std::string& change_summary,
                           const std::string& origin) {
    entry.draft = with_derived_structure(body_md, content_of(*entry.draft));
    append_revision(AppendRevisionArgs{session_id, entry.draft->bodyMd, change_summary, origin});
    // ドラフト行 (一覧の「下書き」) を最新内容に同期する (議題編集で一覧の表題も追従)。
    persist_draft_paper(new_paper_of(session_id, *entry.draft), entry.flow);
    schedule_web_auto_approve_locked(session_id);  // 編集があったら自動開始を延長
}

/** メモリに編集エントリが無い時、永続済みドラフト (status='draft') から復元する。 */
std::shared_ptr<WebPaperReview> rehydrate_draft_entry_locked(const std::string& session_id) {
    const auto row = get_draft_paper(session_id);
    if (!row) return nullptr;
    PaperContent fallback;
    fallback.theme = row->theme;
    fallback.tags = row->tags;
    fallback.supplement = row->supplement;
    fallback.mechanics = row->mechanics;
    auto entry = std::make_shared<WebPaperReview>();
    entry->flow = row->flow;
    entry->ready = true;
    entry->draft = with_derived_structure(row->bodyMd, fallback);
    entry->info = stored_paper_review_info(session_id);
    state().reviews[session_id] = entry;
    return entry;
}

std::shared_ptr<WebPaperReview> find_entry_locked(const std::string& session_id, bool rehydrate) {
    auto& st = state();
    const auto it = st.reviews.find(session_id);
    if (it != st.reviews.end()) return it->second;
    return rehydrate ? rehydrate_draft_entry_locked(session_id) : nullptr;
}

/** セッションの声をペーパーに保存する (使い回し・下書き再開用)。失敗は議論を止めない。 */
void save_session_voices(const std::string& session_id, const std::string& theme, const std::optional<VoiceLookup>& lookup) {
    if (!lookup || !*lookup) return;
    try {
        save_paper_voices(session_id, (*lookup)({theme}, static_cast<int>(PAPER_VOICE_LIMIT)));
    } catch (const std::exception& e) {
        log::warn("[flow-web/paper-voices " + session_id + "] 声の保存失敗 (議論は続行): " + e.what());
    }
}

DispatchDeps base_dispatch_deps(const FlowWebDeps& d) {
    DispatchDeps deps;
    deps.llm = d.llm;
    deps.listExternalVoices = d.listExternalVoices;
    deps.sentimentClients = d.sentimentClients;
    deps.gamesDir = d.gamesDir;
    deps.workspaceId = d.workspaceId;
    return deps;
}

/** 議論/改善を背景で走らせる (FlowRunState を登録し、終わったら finished に入れる)。 */
bool run_flow_in_background(const std::string& session_id, std::function<void(const std::shared_ptr<FlowRunState>&)> body,
                            const std::string& kind) {
    auto run_state = std::make_shared<FlowRunState>();
    {
        std::lock_guard lock(state().m);
        state().running[session_id] = run_state;
    }
    const bool ok = submit_background([session_id, run_state, body = std::move(body), kind]() {
        try {
            body(run_state);
        } catch (const FlowStopped&) {
            log::info("[flow-web] " + kind + " 停止: " + session_id);
        } catch (const std::exception& e) {
            log::warn("[flow-web] " + kind + " 実行エラー: " + e.what());
        }
        std::lock_guard lock(state().m);
        state().finished.insert(session_id);
        const auto it = state().running.find(session_id);
        if (it != state().running.end() && it->second == run_state) state().running.erase(it);
    });
    if (!ok) {
        std::lock_guard lock(state().m);
        state().running.erase(session_id);
    }
    return ok;
}

/** 確定ペーパーで議論を起動する (承認エンドポイント / 自動開始タイマー 共通)。積めなければ false (状態は戻す)。 */
bool start_approved_flow(const std::string& session_id, const std::shared_ptr<WebPaperReview>& entry, const PaperDraft& final_paper,
                         const std::shared_ptr<const FlowWebDeps>& web_deps) {
    std::optional<VoiceLookup> saved_lookup;
    std::optional<Json> debatability;
    FlowKind flow;
    std::optional<double> rounds, turns;
    std::optional<std::vector<std::string>> persona_ids;
    {
        auto& st = state();
        std::lock_guard lock(st.m);
        if (entry->timer) global_scheduler().cancel(*entry->timer);
        entry->timer.reset();
        st.reviews.erase(session_id);
        const auto it = st.voiceLookups.find(session_id);
        if (it != st.voiceLookups.end()) {
            saved_lookup = it->second;
            st.voiceLookups.erase(it);
        }
        if (entry->info) {
            if (const Json* d = field(*entry->info, "debatability"); d && d->is_object()) debatability = *d;
        }
        flow = entry->flow;
        rounds = entry->rounds;
        turns = entry->turnsPerRound;
        persona_ids = entry->personaIds;
    }
    // 使い回した声 > 下書き再開で保存済みの声 > 外部の声検索。議論に使う声を保存し直す。
    // 機密/内部では外部の声を保存も参照もしない。
    const VoiceLookup base_lookup = saved_lookup ? *saved_lookup : session_voice_lookup(session_id, web_deps->listExternalVoices);
    const auto voice_lookup = gate_external_voices(final_paper.tags, base_lookup ? std::optional<VoiceLookup>(base_lookup) : std::nullopt);
    save_session_voices(session_id, final_paper.theme, voice_lookup);

    // 議論適性: 低のまま強行した場合は評価結果を discussion_paper に記録する (09 監査ログ)。
    const bool degraded = debatability && truthy_key(*debatability, "degraded");
    const bool debatable = debatability && truthy_key(*debatability, "debatable");
    if (debatability && !degraded && !debatable) {
        try {
            set_paper_debatability(session_id, debatability);
        } catch (const std::exception& e) {
            log::warn(std::string("[flow-web] debatability 記録失敗 (議論は続行): ") + e.what());
        }
    }

    DispatchInput input;
    input.theme = final_paper.theme;
    input.tags = final_paper.tags;
    input.flow = flow;
    input.rounds = rounds;
    input.turnsPerRound = turns;
    input.personaIds = persona_ids;
    PaperOverride override_;
    override_.mechanics = final_paper.mechanics;
    override_.supplement = final_paper.supplement;
    override_.bodyMd = final_paper.bodyMd;
    // 承認済み論点 (09): ファシリテーター開幕プロンプトの参考として運ぶ。
    override_.issues = final_paper.issues;
    if (debatability && !degraded) {
        DebatabilitySummary s;
        s.debatable = debatable;
        s.armableBothCount = static_cast<int>(json_number(*debatability, "armableBothCount"));
        override_.debatability = s;
    }
    auto deps = base_dispatch_deps(*web_deps);
    deps.listExternalVoices = voice_lookup ? *voice_lookup : ListExternalVoicesFn{};
    deps.sessionId = session_id;
    deps.paperOverride = override_;
    const bool ok = run_flow_in_background(
        session_id,
        [input, deps](const std::shared_ptr<FlowRunState>& run_state) mutable {
            deps.state = run_state;
            dispatch_flow(input, deps);
        },
        flow);
    if (!ok) {
        // 積めなかった: 承認前の状態に戻す (利用者が再度承認できるように)。
        auto& st = state();
        std::lock_guard lock(st.m);
        st.reviews.emplace(session_id, entry);
        if (saved_lookup) st.voiceLookups.emplace(session_id, *saved_lookup);
    }
    return ok;
}

/** 無操作の自動開始タイマーを (張り直して) 仕掛ける。timeoutMs<=0 なら何もしない。 */
void schedule_web_auto_approve_locked(const std::string& session_id) {
    auto& st = state();
    const auto it = st.reviews.find(session_id);
    if (it == st.reviews.end()) return;
    auto entry = it->second;
    if (!entry->ready || !entry->draft) return;
    const double timeout_ms = config::get_config().flow.paperReview.timeoutMs;
    if (!(timeout_ms > 0)) return;
    if (entry->timer) global_scheduler().cancel(*entry->timer);
    std::weak_ptr<WebPaperReview> weak = entry;
    entry->timer = global_scheduler().after(std::chrono::milliseconds(static_cast<long long>(timeout_ms)), [session_id, weak]() {
        // タイマーのスレッドでは重い仕事をしない (背景のプールに渡す)。
        submit_background([session_id, weak]() {
            auto e = weak.lock();
            std::optional<PaperDraft> draft;
            std::shared_ptr<const FlowWebDeps> deps;
            {
                std::lock_guard lock(state().m);
                const auto cur = state().reviews.find(session_id);
                if (!e || cur == state().reviews.end() || cur->second != e || !e->draft) return;
                draft = e->draft;
                deps = state().deps;
                e->timer.reset();
            }
            if (!deps) return;
            log::info("[flow-web/paper " + session_id + "] 無操作のため草案のまま自動開始");
            if (!start_approved_flow(session_id, e, *draft, deps)) log::warn("[flow-web/paper " + session_id + "] 自動開始を積めませんでした");
        });
    });
}

// ─── ルートのハンドラ ────────────────────────────────────────────────────

http::Response handle_start(http::Request& req) {
    const auto web_deps = current_deps();
    if (!web_deps) return not_initialized();
    const Json body = read_body(req, kFlowStartMaxBody);
    const std::string legacy_theme = string_field(body, "theme");
    const std::string additional_spec_text = resolve_additional_spec_text(body, *web_deps);
    const auto base_seed = fixed_seed_from_body(body);
    const auto seed = merge_mechanics_context(base_seed, {raw_string(body, "specText"), additional_spec_text});
    const std::string theme = flow_theme_from_seed(seed, legacy_theme);
    const bool fixed_mode = base_seed.has_value();
    // Anatomia 事前情報: 登録済みプロジェクト名 or リポ絶対パス (どちらか)。config で有効時のみ使う。
    const std::string anatomia_project = string_field(body, "anatomiaProject");
    const std::string anatomia_repo = string_field(body, "anatomiaRepo");
    const std::string flow_label = raw_string(body, "flow").value_or("");
    std::vector<FlowTag> tags;
    if (const Json* t = field(body, "tags"); t && t->is_array())
        for (const auto& x : *t)
            if (x.is_string()) tags.push_back(x.get<std::string>());
    // 議論ごとのラウンド/ターン数 (任意)。run_flow が config 既定にフォールバック + 上限クランプする。
    const auto rounds = read_optional_int(field(body, "rounds"), 1, 10);
    const auto turns_per_round = read_optional_int(field(body, "turnsPerRound"), 1, 20);
    // G: 壁打ち相手 (カンマ区切りの名前/ID)。sparring のみ反映。
    auto persona_ids = parse_persona_ids(field(body, "personaIds"));
    if (!persona_ids) persona_ids = parse_persona_ids(field(body, "opponent"));

    if (theme.empty()) return json_error("テーマは必須です", 400);
    const auto kind_opt = parse_flow_kind(std::string_view(flow_label));
    if (!kind_opt) return json_error("議論タイプ (必須) を選択してください", 400);
    const FlowKind kind = *kind_opt;
    if (fixed_mode && (!seed || seed->discussionTheme.empty() || (requires_project_title(kind) && seed->gameTitle.empty())))
        return json_error("議論テーマは必須です。改善/議論には対象プロジェクト名も必要です", 400);

    const auto dispatch_deps = base_dispatch_deps(*web_deps);

    // 壁打ち: セッションを起動して登録 (対話継続)
    if (kind == "sparring") {
        DispatchInput in;
        in.theme = theme;
        in.tags = tags;
        in.flow = kind;
        in.scene = "web:flow-" + crypto::uuid_v4().substr(0, 8);
        in.opponentPersonaIds = persona_ids;
        const auto result = dispatch_flow(in, dispatch_deps);
        if (result.kind != "sparring" || !result.session) return json_error("internal", 500);
        {
            std::lock_guard lock(state().m);
            state().sparring[result.session->session_id()] = result.session;
        }
        return http::Response::json(Json{{"ok", true}, {"kind", kind}, {"sessionId", result.session->session_id()}});
    }

    // 学習: 短時間で完走するため待って結果を返す。
    if (kind == "learning") {
        if (!web_deps->openCore) return json_error("learning は Core 未設定のため不可", 400);
        const auto youtube_api_key = resolve_youtube_api_key(*web_deps);
        const auto learning_crawl = build_learning_crawl(body, youtube_api_key);
        std::string spec_text = seed ? str::trim(seed->mechanicsContext) : "";
        if (spec_text.empty()) spec_text = string_field(body, "specText");
        auto core = web_deps->openCore();
        struct Closer {
            core::Core* c;
            ~Closer() {
                if (c) c->close();
            }
        } closer{core.get()};
        std::vector<GameMechanicEntry> mechanics;
        if (!spec_text.empty()) {
            AnalyzeSpecMechanicsArgs a;
            a.theme = theme;
            a.specText = spec_text;
            a.warn = [](const std::string& m) { log::warn("[flow-web/spec] " + m); };
            mechanics = analyze_spec_mechanics(*web_deps->llm, a);
        }
        const auto extra = resolve_anatomia_extra_mechanics(theme, anatomia_project, anatomia_repo, *web_deps,
                                                            [](const std::string& m) { log::warn("[flow-web/anatomia learning] " + m); });
        if (extra)
            for (const auto& m : *extra) mechanics.push_back(mechanic_summary_to_entry(m));
        DispatchInput in;
        in.theme = theme;
        in.tags = tags;
        in.flow = kind;
        auto deps = dispatch_deps;
        deps.core = core.get();
        deps.learningCrawl = learning_crawl;
        deps.mechanics = mechanics;
        const auto result = dispatch_flow(in, deps);
        if (result.kind != "learning" || !result.learning) return json_error("internal", 500);
        auto final_result = *result.learning;
        std::size_t synthetic_count = 0;
        if (final_result.opinionsRecorded == 0 && final_result.crawledImported == 0) {
            auto opinions = synthesize_learning_opinions(theme, mechanics, spec_text, *web_deps->llm,
                                                         [](const std::string& m) { log::warn("[flow-web/learning] " + m); });
            synthetic_count = opinions.size();
            if (!opinions.empty()) {
                auto sdeps = dispatch_deps;
                sdeps.core = core.get();
                sdeps.opinions = std::move(opinions);
                const auto synthetic = dispatch_flow(in, sdeps);
                if (synthetic.kind == "learning" && synthetic.learning) {
                    final_result.opinionsRecorded += synthetic.learning->opinionsRecorded;
                    Json pb = Json::object();
                    for (const char* k : {"positive", "negative", "neutral"})
                        pb[k] = json_number(final_result.polarityBreakdown, k) + json_number(synthetic.learning->polarityBreakdown, k);
                    final_result.polarityBreakdown = pb;
                }
            }
        }
        return http::Response::json(Json{{"ok", true},
                                         {"kind", kind},
                                         {"sessionId", final_result.gameSlug},
                                         {"result", to_json(final_result)},
                                         {"syntheticOpinions", synthetic_count}});
    }

    // 議論 / 改善: sessionId を先に発番し、バックグラウンドで完走させてポーリングで追う。
    const std::string session_id = crypto::uuid_v4();
    const auto auto_crawl_spec = build_auto_crawl_spec(body);
    // ペーパーの使い回し (paper-reuse.md): 欄の番号 > 本文中の「ペーパー#N」。
    std::optional<std::int64_t> reuse_no;
    if (auto n = read_optional_int(field(body, "reusePaperNo"), 1, kMaxSafeInteger)) reuse_no = static_cast<std::int64_t>(*n);
    else
        reuse_no = parse_reuse_paper_no(legacy_theme + "\n" + (seed ? seed->discussionContent : "") + "\n" + (seed ? seed->mechanicsContext : ""));
    std::optional<ReusablePaper> reused;
    if (reuse_no) {
        reused = load_reusable_paper(*reuse_no);
        log::info("[flow-web " + session_id + "] " + describe_reuse(*reuse_no, reused));
    }
    VoiceLookup voice_lookup = web_deps->listExternalVoices;
    if (reused && !reused->voices.empty()) {
        voice_lookup = stored_voice_lookup(reused->voices);
        std::lock_guard lock(state().m);
        state().voiceLookups[session_id] = voice_lookup;
    }

    // ペーパー編集ゲート (Web 正規フロー): 情報整備後に草案を作り、ブラウザの編集/承認を待つ。
    if (web_paper_gate_enabled()) {
        auto entry = std::make_shared<WebPaperReview>();
        entry->flow = kind;
        entry->rounds = rounds;
        entry->turnsPerRound = turns_per_round;
        entry->personaIds = persona_ids;
        {
            std::lock_guard lock(state().m);
            state().reviews[session_id] = entry;
        }
        const bool ok = submit_background([=]() {
            const std::string prefix = "[flow-web/paper " + session_id + "] ";
            try {
                // 声を使い回すときは集め直さない (情報ゲート・自動クロールを省く)。
                if (!reused || reused->voices.empty())
                    prepare_information_before_flow(kind, theme, tags, session_id, auto_crawl_spec, *web_deps);
                const auto& richness = config::get_config().flow.paperRichness;
                // Anatomia 事前情報: 対象が指定され config 有効なら、ソース解析由来のメカニクスを先頭に差す。
                const auto extra = resolve_anatomia_extra_mechanics(theme, anatomia_project, anatomia_repo, *web_deps, [session_id](const std::string& m) {
                    log::warn("[flow-web/anatomia " + session_id + "] " + m);
                });
                const auto reused_context = merge_reused_context(reused, seed ? std::optional<std::string>(seed->mechanicsContext) : std::nullopt);
                const auto debatability_gate = resolve_debatability_gate(kind, session_id, *web_deps->llm);
                BuildPaperDraftDeps bd;
                bd.flow = kind;
                bd.gamesDir = web_deps->gamesDir;
                if (voice_lookup) bd.listExternalVoices = voice_lookup;
                bd.llm = richness.enrichMechanics ? web_deps->llm.get() : nullptr;
                bd.understandingLlm = web_deps->llm.get();
                bd.mechanicsTarget = static_cast<int>(richness.mechanicsTarget);
                if (!richness.enrichModel.empty()) bd.enrichModel = richness.enrichModel;
                if (reused && !reused->mechanics.empty()) {
                    bd.extraMechanics = reused->mechanics;
                    if (extra) bd.extraMechanics.insert(bd.extraMechanics.end(), extra->begin(), extra->end());
                } else if (extra) {
                    bd.extraMechanics = *extra;
                }
                if (reused_context) {
                    PaperFixedFields s = seed ? *seed : PaperFixedFields{};
                    s.mechanicsContext = *reused_context;
                    bd.seed = s;
                } else {
                    bd.seed = seed;
                }
                // 議論適性ゲート (09): 情報ゲートの後段・人間レビューの前 (無効時は nullopt = 現行挙動)。
                if (debatability_gate) bd.debatability = debatability_gate->to_paper_gate();
                bd.warn = [prefix](const std::string& m) { log::warn(prefix + m); };
                const auto built = build_paper_draft(theme, tags, bd);
                Json info = to_json(built.info);
                info["voiceSimulation"] = assess_voice_simulation_readiness(built.draft, info, kind);
                info["fixSuggestions"] =
                    suggest_fixes_for_paper(kind, built.draft, info, *web_deps->llm, [prefix](const std::string& m) { log::warn(prefix + m); });
                std::lock_guard lock(state().m);
                const auto it = state().reviews.find(session_id);
                if (it == state().reviews.end() || it->second != entry) return;
                entry->ready = true;
                entry->draft = built.draft;
                entry->info = info;
                // 初期草案を版履歴の rev1 として記録 (Notion 風編集の「戻す」基点)。
                append_revision(AppendRevisionArgs{session_id, built.draft.bodyMd, "初期草案", "initial"});
                // ドラフトを discussion_paper(status='draft') に永続 → 議論一覧に「下書き」として出す/再開できる。
                persist_draft_paper(new_paper_of(session_id, built.draft), kind);
                save_paper_review_info(session_id, info);
                save_session_voices(session_id, theme, voice_lookup ? std::optional<VoiceLookup>(voice_lookup) : std::nullopt);
                schedule_web_auto_approve_locked(session_id);  // 無操作タイムアウト (timeoutMs>0 時のみ)
            } catch (const std::exception& e) {
                {
                    std::lock_guard lock(state().m);
                    const auto it = state().reviews.find(session_id);
                    if (it != state().reviews.end()) {
                        it->second->ready = true;
                        it->second->error = e.what();
                    }
                }
                log::warn("[flow-web] " + kind + " ペーパー草案エラー: " + e.what());
            }
        });
        if (!ok) {
            std::lock_guard lock(state().m);
            state().reviews.erase(session_id);
            state().voiceLookups.erase(session_id);
            return json_error(kBusyMessage, 503);
        }
        return http::Response::json(Json{{"ok", true}, {"kind", kind}, {"sessionId", session_id}, {"review", true}});
    }

    DispatchInput in;
    in.theme = theme;
    in.tags = tags;
    in.flow = kind;
    in.rounds = rounds;
    in.turnsPerRound = turns_per_round;
    in.personaIds = persona_ids;
    auto deps = dispatch_deps;
    deps.sessionId = session_id;
    const bool ok = run_flow_in_background(
        session_id,
        [=](const std::shared_ptr<FlowRunState>& run_state) mutable {
            prepare_information_before_flow(kind, theme, tags, session_id, auto_crawl_spec, *web_deps);
            deps.state = run_state;
            dispatch_flow(in, deps);
        },
        kind);
    if (!ok) {
        std::lock_guard lock(state().m);
        state().voiceLookups.erase(session_id);
        return json_error(kBusyMessage, 503);
    }
    return http::Response::json(Json{{"ok", true}, {"kind", kind}, {"sessionId", session_id}});
}

http::Response handle_get_paper(http::Request& req) {
    const std::string session_id = req.param("session");
    std::lock_guard lock(state().m);
    const auto entry = find_entry_locked(session_id, true);
    if (!entry) {
        if (const auto persisted = get_paper_snapshot(session_id)) {
            const auto info = stored_paper_review_info(session_id);
            Json j = Json::object();
            j["ok"] = true;
            j["ready"] = false;
            j["started"] = persisted->status != "draft";
            j["status"] = persisted->status;
            j["error"] = nullptr;
            j["info"] = info ? *info : Json(nullptr);
            j["paper"] = nullptr;
            j["blocks"] = Json::array();
            j["paperMd"] = persisted->bodyMd;
            return http::Response::json(j);
        }
        return http::Response::json(Json{{"ok", true},
                                         {"ready", false},
                                         {"missing", true},
                                         {"error", nullptr},
                                         {"info", nullptr},
                                         {"paper", nullptr},
                                         {"blocks", Json::array()}});
    }
    Json j = Json::object();
    j["ok"] = true;
    j["ready"] = entry->ready;
    j["started"] = false;
    j["missing"] = false;
    j["error"] = entry->error ? Json(*entry->error) : Json(nullptr);
    if (entry->ready && entry->draft) {
        const Json payload = paper_payload_locked(session_id, *entry->draft, entry.get());
        for (auto it = payload.begin(); it != payload.end(); ++it) j[it.key()] = it.value();
    } else {
        j["info"] = entry->info ? *entry->info : Json(nullptr);
        j["paper"] = nullptr;
        j["blocks"] = Json::array();
    }
    return http::Response::json(j);
}

http::Response not_ready(const char* message = "編集準備中です") { return json_error(message, 409); }

http::Response handle_form_apply(http::Request& req) {
    if (!current_deps()) return not_initialized();
    const std::string session_id = req.param("session");
    const Json body = read_body(req, kFlowEditMaxBody);
    std::lock_guard lock(state().m);
    const auto entry = find_entry_locked(session_id, false);
    if (!entry || !entry->ready || !entry->draft) return not_ready();
    const auto& draft = *entry->draft;
    const auto current = paper_fixed_fields_from_markdown(draft.bodyMd, content_of(draft));
    PaperFixedFields next;
    next.gameTitle = has_key(body, "gameTitle") ? string_field(body, "gameTitle") : current.gameTitle;
    next.discussionTheme = string_field(body, "discussionTheme");
    if (next.discussionTheme.empty()) next.discussionTheme = current.discussionTheme;
    next.discussionContent = has_key(body, "discussionContent") ? string_field(body, "discussionContent") : current.discussionContent;
    next.mechanicsContext = has_key(body, "mechanicsContext") ? string_field(body, "mechanicsContext") : current.mechanicsContext;
    next.themeSupplement = has_key(body, "themeSupplement") ? string_field(body, "themeSupplement") : current.themeSupplement;
    if (next.discussionTheme.empty() || (requires_project_title(entry->flow) && next.gameTitle.empty()))
        return json_error("議論テーマは必須です。改善/議論には対象プロジェクト名も必要です", 400);
    const auto tags = has_key(body, "tags") ? sanitize_review_tags(field(body, "tags"), draft.tags) : draft.tags;
    if (has_key(body, "rounds")) entry->rounds = read_optional_int(field(body, "rounds"), 1, 10);
    if (has_key(body, "turnsPerRound")) entry->turnsPerRound = read_optional_int(field(body, "turnsPerRound"), 1, 20);
    // 論点 (09): 1 行 1 論点のテキスト or 配列で受ける (フィールド未指定なら現行値を保持)。
    const auto issues = has_key(body, "issues") ? parse_issues_field(field(body, "issues")) : draft.issues;

    PaperContent c = content_of(draft);
    c.gameTitle = next.gameTitle;
    c.discussionTheme = next.discussionTheme;
    c.discussionContent = next.discussionContent;
    c.mechanicsContext = next.mechanicsContext;
    c.themeSupplement = next.themeSupplement;
    c.theme = next.discussionTheme;
    c.tags = tags;
    c.supplement = next.themeSupplement;
    c.issues = issues;
    commit_body_md_locked(session_id, *entry, paper_draft_to_markdown(c), "固定フォーム保存", "manual");
    return http::Response::json(ok_with(paper_payload_locked(session_id, *entry->draft, entry.get())));
}

http::Response handle_debatability_check(http::Request& req) {
    const auto web_deps = current_deps();
    if (!web_deps) return not_initialized();
    const std::string session_id = req.param("session");
    std::optional<PaperDraft> draft;
    std::optional<Json> entry_info;
    FlowKind flow;
    std::shared_ptr<WebPaperReview> entry;
    {
        std::lock_guard lock(state().m);
        entry = find_entry_locked(session_id, true);
        if (!entry || !entry->ready || !entry->draft) return not_ready();
        draft = entry->draft;
        flow = entry->flow;
    }
    const auto gate = manual_debatability_gate(flow, session_id, *web_deps->llm);
    if (!gate) return json_error("このフローでは議論可能性チェックを実行できません", 400);
    const auto prefix = "[flow-web/paper " + session_id + "] ";
    auto warn = [prefix](const std::string& m) { log::warn(prefix + m); };

    const auto list_voices = gate_external_voices(
        draft->tags, web_deps->listExternalVoices ? std::optional<ListExternalVoicesFn>(web_deps->listExternalVoices) : std::nullopt);
    const std::vector<ContextVoice> voices = list_voices ? (*list_voices)({draft->theme}, DEBATABILITY_VOICE_LIMIT) : std::vector<ContextVoice>{};
    AssessDebatabilityArgs a;
    a.theme = draft->theme;
    a.paperMd = paper_md_with_premise(flow, draft->bodyMd);
    for (const auto& v : voices) a.voices.push_back(DebatabilityVoice{v.content, v.source});
    a.minArmableIssues = gate->minArmableIssues;
    a.warn = warn;
    const auto result = assess_debatability(*gate->llm, a);

    {
        std::lock_guard lock(state().m);
        entry_info = entry->info;
    }
    Json info = default_paper_info(entry_info);
    info["voiceCount"] = voices.size();
    info["countCapped"] = voices.size() >= static_cast<std::size_t>(DEBATABILITY_VOICE_LIMIT);
    info["samples"] = review_info_samples(voices);
    info["debatability"] = to_json(result);
    info["voiceSimulation"] = assess_voice_simulation_readiness(*draft, info, flow);
    info["fixSuggestions"] = suggest_fixes_for_paper(flow, *draft, info, *web_deps->llm, warn);

    std::lock_guard lock(state().m);
    entry->info = info;
    save_paper_review_info(session_id, info);
    const auto issues = annotated_issues(result);
    if (!issues.empty() && entry->draft) {
        PaperContent c = content_of(*entry->draft);
        c.issues = issues;
        commit_body_md_locked(session_id, *entry, paper_draft_to_markdown(c), "議論可能性チェック", "manual");
    }
    return http::Response::json(ok_with(paper_payload_locked(session_id, *entry->draft, entry.get())));
}

http::Response handle_mechanics_check(http::Request& req) {
    const auto web_deps = current_deps();
    if (!web_deps) return not_initialized();
    const std::string session_id = req.param("session");
    std::shared_ptr<WebPaperReview> entry;
    std::optional<PaperDraft> draft;
    {
        std::lock_guard lock(state().m);
        entry = find_entry_locked(session_id, true);
        if (!entry || !entry->ready || !entry->draft) return not_ready();
        draft = entry->draft;
    }
    const auto fields = paper_fixed_fields_from_markdown(draft->bodyMd, content_of(*draft));
    const auto prefix = "[flow-web/paper " + session_id + "] ";
    const Json knowledge = check_mechanics_knowledge(*draft, fields, *web_deps->llm, [prefix](const std::string& m) { log::warn(prefix + m); });
    std::lock_guard lock(state().m);
    Json info = default_paper_info(entry->info);
    info["mechanicsKnowledge"] = knowledge;
    info["voiceSimulation"] = assess_voice_simulation_readiness(*draft, info, entry->flow);
    entry->info = info;
    save_paper_review_info(session_id, info);
    return http::Response::json(ok_with(paper_payload_locked(session_id, *entry->draft, entry.get())));
}

http::Response handle_fix_apply(http::Request& req) {
    if (!current_deps()) return not_initialized();
    const std::string session_id = req.param("session");
    const Json body = read_body(req, kFlowEditMaxBody);
    std::lock_guard lock(state().m);
    const auto entry = find_entry_locked(session_id, true);
    if (!entry || !entry->ready || !entry->draft) return not_ready();
    long long index = -1;
    if (const Json* si = field(body, "suggestionIndex"); si && si->is_number()) {
        const double v = si->get<double>();
        if (std::isfinite(v) && std::trunc(v) == v && std::fabs(v) < 1e15) index = static_cast<long long>(v);
    }
    Json info = default_paper_info(entry->info);
    const Json* suggestions = field(info, "fixSuggestions");
    if (!suggestions || !suggestions->is_array() || index < 0 || static_cast<std::size_t>(index) >= suggestions->size() ||
        !json_truthy((*suggestions)[static_cast<std::size_t>(index)]))
        return json_error("修正提案が見つかりません", 404);
    const Json suggestion = (*suggestions)[static_cast<std::size_t>(index)];
    if (suggestion.is_object() && suggestion.contains("appliedAt") && json_truthy(suggestion["appliedAt"]))
        return json_error("この修正提案は反映済みです", 409);
    std::string text = string_field(body, "text");
    if (text.empty()) text = json_string(suggestion, "suggestedChange");
    text = str::trim(text);
    if (text.empty()) return json_error("追記内容が空です", 400);
    if (str::js_length(text) > 8000) return json_error("追記内容が長すぎます", 400);

    // appendFixSuggestionToPaper
    const auto& draft = *entry->draft;
    const auto fields = paper_fixed_fields_from_markdown(draft.bodyMd, content_of(draft));
    std::string title = str::trim(json_string(suggestion, "title"));
    if (title.empty()) title = "修正提案";
    const std::string addition = "## 修正追記: " + title + "\n" + str::trim(text);
    const std::string discussion_content =
        str::trim(fields.discussionContent).empty() ? addition : str::trim_end(fields.discussionContent) + "\n\n" + addition;
    PaperContent c = content_of(draft);
    c.gameTitle = fields.gameTitle;
    c.discussionTheme = fields.discussionTheme;
    c.discussionContent = discussion_content;
    c.mechanicsContext = fields.mechanicsContext;
    c.themeSupplement = fields.themeSupplement;
    c.theme = fields.discussionTheme.empty() ? draft.theme : fields.discussionTheme;
    c.supplement = fields.themeSupplement.empty() ? draft.supplement : fields.themeSupplement;
    commit_body_md_locked(session_id, *entry, paper_draft_to_markdown(c), "修正提案を追記: " + json_string(suggestion, "title"), "manual");
    Json& target = info["fixSuggestions"][static_cast<std::size_t>(index)];
    if (target.is_object()) {
        target["appliedAt"] = time::now_ms();
        target["appliedText"] = text;
    }
    entry->info = info;
    save_paper_review_info(session_id, info);
    return http::Response::json(ok_with(paper_payload_locked(session_id, *entry->draft, entry.get())));
}

http::Response handle_block_review(http::Request& req) {
    const auto web_deps = current_deps();
    if (!web_deps) return not_initialized();
    const std::string session_id = req.param("session");
    std::optional<PaperDraft> draft;
    {
        std::lock_guard lock(state().m);
        const auto entry = find_entry_locked(session_id, false);
        if (!entry || !entry->ready || !entry->draft) return not_ready();
        draft = entry->draft;
    }
    const Json body = read_body(req, kFlowSmallMaxBody);
    const std::string block_id = raw_string(body, "blockId").value_or("");
    if (block_id.empty()) return json_error("blockId が必要です", 400);
    ReviewBlockArgs a;
    a.bodyMd = draft->bodyMd;
    a.blockId = block_id;
    a.instruction = raw_string(body, "instruction");
    a.model = model_or_null();
    const auto prefix = "[flow-web/paper " + session_id + "] ";
    a.warn = [prefix](const std::string& m) { log::warn(prefix + m); };
    const auto result = review_block(*web_deps->llm, a);
    // result.ok (改稿成否) は envelope の ok と衝突するので reviewed に分離する。
    Json rest = to_json(result);
    rest.erase("ok");
    return http::Response::json(ok_with(rest, {{"reviewed", result.ok}}));
}

http::Response handle_block_apply(http::Request& req) {
    if (!current_deps()) return not_initialized();
    const std::string session_id = req.param("session");
    const Json body = read_body(req, kFlowEditMaxBody);
    std::lock_guard lock(state().m);
    const auto entry = find_entry_locked(session_id, false);
    if (!entry || !entry->ready || !entry->draft) return not_ready();
    const std::string block_id = raw_string(body, "blockId").value_or("");
    const std::string new_text = raw_string(body, "newText").value_or("");
    if (block_id.empty()) return json_error("blockId が必要です", 400);
    const std::string next_md = replace_block(entry->draft->bodyMd, block_id, new_text);
    std::string summary = string_field(body, "summary");
    if (summary.empty()) summary = "ブロックを編集";
    commit_body_md_locked(session_id, *entry, next_md, summary, "manual");
    return http::Response::json(ok_with(paper_payload_locked(session_id, *entry->draft, entry.get())));
}

/** (text).replace(/^#{1,6}\s+/, "") */
std::string strip_heading_marker(const std::string& s) {
    std::size_t n = 0;
    while (n < s.size() && s[n] == '#') ++n;
    if (n < 1 || n > 6) return s;
    const std::size_t q = skip_js_spaces(s, n);
    if (q == n) return s;
    return s.substr(q);
}

http::Response handle_crawl(http::Request& req) {
    const auto web_deps = current_deps();
    if (!web_deps) return not_initialized();
    const std::string session_id = req.param("session");
    std::shared_ptr<WebPaperReview> entry;
    std::optional<PaperDraft> draft;
    {
        std::lock_guard lock(state().m);
        entry = find_entry_locked(session_id, false);
        if (!entry || !entry->ready || !entry->draft) return not_ready();
        draft = entry->draft;
    }
    if (!web_deps->listExternalVoices) return json_error("外部の声の参照が無効です", 400);
    if (!can_collect_external(draft->tags)) return json_error("機密/内部タグの議論では外部の声を参照しません", 400);
    const Json body = read_body(req, kFlowSmallMaxBody);
    const std::string block_id = raw_string(body, "blockId").value_or("");
    const std::string query = string_field(body, "query");
    // 検索語: 明示 query > ブロック本文 > 議題。
    std::string topic = query;
    if (topic.empty()) {
        std::string block_text;
        if (!block_id.empty())
            for (const auto& b : split_blocks(draft->bodyMd))
                if (b.id == block_id) {
                    block_text = b.text;
                    break;
                }
        topic = str::js_slice(strip_heading_marker(block_text), 0, 80);
    }
    if (topic.empty()) topic = draft->theme;
    GatherEvidenceArgs a;
    a.topic = topic;
    a.listExternalVoices = web_deps->listExternalVoices;
    a.llm = web_deps->llm.get();
    a.model = model_or_null();
    const auto prefix = "[flow-web/paper " + session_id + "] ";
    a.warn = [prefix](const std::string& m) { log::warn(prefix + m); };
    const auto evidence = gather_evidence(a);
    const Json* insert = field(body, "insert");
    // insert=true なら提案段落を対象ブロック直後 (無ければ末尾) に挿入して確定。
    if (insert && insert->is_boolean() && insert->get<bool>() && !evidence.suggestion.empty()) {
        std::lock_guard lock(state().m);
        if (!entry->draft) return not_ready();
        const std::string next_md = !block_id.empty() ? insert_block_after(entry->draft->bodyMd, block_id, evidence.suggestion)
                                                      : entry->draft->bodyMd + "\n\n" + evidence.suggestion;
        commit_body_md_locked(session_id, *entry, next_md, "根拠を追記 (" + std::to_string(evidence.voices.size()) + " 件)", "crawl");
        return http::Response::json(
            ok_with(paper_payload_locked(session_id, *entry->draft, entry.get()), {{"inserted", true}, {"evidence", to_json(evidence)}}));
    }
    return http::Response::json(Json{{"ok", true}, {"inserted", false}, {"evidence", to_json(evidence)}});
}

http::Response handle_revert(http::Request& req) {
    if (!current_deps()) return not_initialized();
    const std::string session_id = req.param("session");
    std::lock_guard lock(state().m);
    const auto entry = find_entry_locked(session_id, false);
    if (!entry || !entry->ready || !entry->draft) return not_ready();
    const auto reverted = revert_last(session_id);
    if (!reverted) return json_error("これ以上戻せません", 409);
    entry->draft = with_derived_structure(reverted->bodyMd, content_of(*entry->draft));
    schedule_web_auto_approve_locked(session_id);
    return http::Response::json(
        ok_with(paper_payload_locked(session_id, *entry->draft, entry.get()), {{"changeSummary", reverted->changeSummary}}));
}

http::Response handle_approve(http::Request& req) {
    const auto web_deps = current_deps();
    if (!web_deps) return not_initialized();
    const std::string session_id = req.param("session");
    std::shared_ptr<WebPaperReview> entry;
    std::optional<PaperDraft> draft;
    {
        std::lock_guard lock(state().m);
        entry = find_entry_locked(session_id, false);
        if (!entry || !entry->ready || !entry->draft) return not_ready("レビュー準備中です");
        draft = entry->draft;
    }
    const Json body = read_body(req, kFlowApproveMaxBody);
    // body.paper があれば Web フォームの直接編集を確定値に採用 (なければ蓄積済みドラフト)。
    const Json* paper = field(body, "paper");
    const PaperDraft final_paper = coerce_paper_draft(paper ? *paper : Json(nullptr), *draft);
    if (!start_approved_flow(session_id, entry, final_paper, web_deps)) return json_error(kBusyMessage, 503);
    return http::Response::json(Json{{"ok", true}, {"sessionId", session_id}});
}

http::Response handle_say(http::Request& req) {
    const std::string session_id = req.param("session");
    std::shared_ptr<SparringSession> session;
    {
        std::lock_guard lock(state().m);
        const auto it = state().sparring.find(session_id);
        if (it != state().sparring.end()) session = it->second;
    }
    if (!session) return json_error("セッションが見つかりません", 404);
    const Json body = read_body(req, kFlowSmallMaxBody);
    const std::string text = string_field(body, "text");
    if (text.empty()) return json_error("発話が空です", 400);
    const auto result = session->submit_user(text);
    if (result.kind == "ended") {
        std::lock_guard lock(state().m);
        state().finished.insert(session_id);
    }
    return http::Response::json(Json{{"ok", true}, {"result", Json{{"kind", result.kind}}}});
}

double clamp_int_query(const std::optional<std::string>& v, double fallback, double min, double max) {
    const auto n = api::js_query::parse_int(v.value_or(""));
    if (!n) return fallback;
    return std::min(max, std::max(min, *n));
}

http::Response handle_sessions(http::Request& req) {
    const auto state_q = req.query("state").value_or("");
    const std::string st = state_q == "draft" || state_q == "live" || state_q == "concluded" ? state_q : "all";
    const auto scope_q = req.query("scope").value_or("");
    const std::string scope = scope_q == "ai" || scope_q == "chat" ? scope_q : "all";
    const auto limit = static_cast<std::int64_t>(clamp_int_query(req.query("limit"), 100, 1, 200));
    const auto offset = static_cast<std::int64_t>(clamp_int_query(req.query("offset"), 0, 0, kMaxSafeInteger));
    ListFlowSessionsOptions o;
    o.state = st;
    o.scope = scope;
    o.limit = limit;
    o.offset = offset;
    const auto res = list_flow_sessions(o);
    Json sessions = Json::array();
    for (const auto& r : res.rows) {
        Json row = to_json(r);
        const bool concluded = r.concluded && *r.concluded == 1;
        // 表示状態: draft (編集中・未確定) / concluded (結論あり) / live (進行/未収束)。
        row["concluded"] = concluded;
        row["state"] = r.status == "draft" ? "draft" : concluded ? "concluded" : "live";
        sessions.push_back(std::move(row));
    }
    Json j = Json::object();
    j["ok"] = true;
    j["total"] = res.total;
    j["limit"] = limit;
    j["offset"] = offset;
    j["scope"] = scope;
    j["hasMore"] = offset + static_cast<std::int64_t>(res.rows.size()) < res.total;
    j["sessions"] = sessions;
    return http::Response::json(j);
}

http::Response handle_delete(http::Request& req) {
    const std::string session_id = req.param("session");
    {
        // 進行中セッションのインメモリ状態を先に片付ける (タイマー/壁打ち/完了フラグ/進行中の議論)。
        auto& st = state();
        std::lock_guard lock(st.m);
        const auto it = st.reviews.find(session_id);
        if (it != st.reviews.end()) {
            if (it->second->timer) global_scheduler().cancel(*it->second->timer);
            st.reviews.erase(it);
        }
        st.sparring.erase(session_id);
        st.finished.erase(session_id);
        st.voiceLookups.erase(session_id);
        // C++ 版の追加: 進行中の議論を次の区切りで止める (削除した議論に発話を書き続けない)。
        const auto run = st.running.find(session_id);
        if (run != st.running.end()) run->second->request_stop();
    }
    if (!delete_flow_session(session_id)) return json_error("対象の議論が見つかりません", 404);
    return http::Response::json(Json{{"ok", true}, {"sessionId", session_id}});
}

std::vector<std::string> string_array_column(const std::string& raw) {
    std::vector<std::string> out;
    const auto parsed = parse_json(raw, kDbJsonLimits);
    if (parsed && parsed->is_array())
        for (const auto& v : *parsed)
            if (v.is_string()) out.push_back(v.get<std::string>());
    return out;
}

http::Response handle_status(http::Request& req) {
    const std::string session_id = req.param("session");
    double since = 0;
    if (const auto n = api::js_query::parse_int(req.query("since").value_or("0")); n && *n != 0) since = *n;
    auto db = db::get_flow_db();

    const auto utterances = db->all(
        "SELECT id, round, turn, persona_name, role, stance, possession_name, text, is_error, created_at\n"
        "         FROM flow_utterance\n"
        "        WHERE session_id = ? AND created_at > ?\n"
        "        ORDER BY created_at ASC",
        {session_id, since}, kStatusRowLimits);

    const auto vote_rows = db->all(
        "SELECT round, chosen_utterance_id AS id, COUNT(*) AS votes\n"
        "         FROM vote\n"
        "        WHERE session_id = ? AND chosen_utterance_id IS NOT NULL\n"
        "        GROUP BY round, chosen_utterance_id\n"
        "        ORDER BY round ASC",
        {session_id}, kStatusRowLimits);
    std::map<std::string, std::int64_t> vote_by_id;
    std::map<std::int64_t, std::pair<std::string, std::int64_t>> vote_winner_by_round;
    for (const auto& r : vote_rows) {
        const auto id = r.str("id");
        const auto votes = r.i64("votes");
        vote_by_id[id] += votes;
        const auto round = r.i64("round");
        const auto cur = vote_winner_by_round.find(round);
        if (cur == vote_winner_by_round.end() || votes > cur->second.second) vote_winner_by_round[round] = {id, votes};
    }
    const auto score_winner_rows = db->all(
        "SELECT round, utterance_id AS id\n"
        "         FROM improvement_score\n"
        "        WHERE session_id = ? AND is_winner = 1",
        {session_id}, kStatusRowLimits);
    const auto top_row = db->get("SELECT top_utterance_ids_json FROM flow_conclusion WHERE session_id = ?", {session_id});
    std::set<std::string> winner_ids;
    if (top_row)
        for (auto& id : string_array_column(top_row->str("top_utterance_ids_json"))) winner_ids.insert(std::move(id));
    for (const auto& [round, w] : vote_winner_by_round)
        if (w.second > 0) winner_ids.insert(w.first);
    for (const auto& r : score_winner_rows) winner_ids.insert(r.str("id"));
    const auto round_rows = db->all(
        "SELECT round, aufhebung_json FROM discussion_paper_round WHERE paper_id IN (SELECT id FROM discussion_paper WHERE session_id = ?)",
        {session_id}, kStatusRowLimits);
    std::map<std::int64_t, std::vector<std::string>> aufhebung_by_round;
    for (const auto& r : round_rows) aufhebung_by_round[r.i64("round")] = string_array_column(r.str("aufhebung_json"));
    const auto mark_rows = db->all("SELECT id, round FROM flow_utterance WHERE session_id = ?", {session_id}, kStatusRowLimits);
    auto mark_fields = [&](Json& out, const std::string& id, std::int64_t round) {
        const auto v = vote_by_id.find(id);
        const std::int64_t votes = v == vote_by_id.end() ? 0 : v->second;
        const bool is_winner = winner_ids.count(id) > 0;
        Json auf = Json::array();
        // 止揚はラウンド全体ではなく、世論として使われた意見だけに付ける。
        if (is_winner) {
            const auto a = aufhebung_by_round.find(round);
            if (a != aufhebung_by_round.end()) auf = a->second;
        }
        out["votes"] = votes;
        out["isWinner"] = is_winner;
        out["roundAufhebung"] = auf;
        return votes > 0 || is_winner || !auf.empty();
    };

    const auto conclusion_row = db->get("SELECT summary, concluded FROM flow_conclusion WHERE session_id = ?", {session_id});

    bool finished = false;
    std::shared_ptr<SparringSession> session;
    {
        std::lock_guard lock(state().m);
        finished = state().finished.count(session_id) > 0;
        const auto it = state().sparring.find(session_id);
        if (it != state().sparring.end()) session = it->second;
    }
    // 結論の行は未来予測より前に書かれるので、未来予測が待機中 / 実行中の間は完了にしない。
    const auto foresight = foresight::get_foresight_status(session_id);
    const bool done = (finished || (session && session->is_ended()) || conclusion_row.has_value()) && !foresight::is_foresight_active(session_id);

    Json out_utterances = Json::array();
    for (const auto& u : utterances) {
        Json j = Json::object();
        const std::string id = u.str("id");
        j["id"] = id;
        j["round"] = u["round"].to_json();
        j["turn"] = u["turn"].to_json();
        j["personaName"] = u.str("persona_name");
        // 「名前 (ロール/憑依ペルソナ)」(item2/4)。憑依なしは括弧内ロールのみ。
        DisplayNameInput dn;
        dn.name = u.str("persona_name");
        dn.stance = u.opt_str("stance");
        dn.role = u.opt_str("role");
        dn.possessionName = u.opt_str("possession_name");
        j["displayName"] = compose_display_name(dn);
        j["role"] = u["role"].to_json();
        j["stance"] = u["stance"].to_json();
        j["possessionName"] = u["possession_name"].to_json();
        j["text"] = u["text"].to_json();
        j["isError"] = u.i64("is_error") == 1;
        mark_fields(j, id, u.i64("round"));
        j["createdAt"] = u["created_at"].to_json();
        out_utterances.push_back(std::move(j));
    }
    Json marks = Json::array();
    for (const auto& r : mark_rows) {
        Json m = Json::object();
        const std::string id = r.str("id");
        m["id"] = id;
        if (mark_fields(m, id, r.i64("round"))) marks.push_back(std::move(m));
    }
    Json j = Json::object();
    j["ok"] = true;
    j["utterances"] = out_utterances;
    j["marks"] = marks;
    j["conclusion"] = conclusion_row ? (*conclusion_row)["summary"].to_json() : Json(nullptr);
    j["concluded"] = conclusion_row && conclusion_row->i64("concluded") == 1;
    // ライブのディスカッションペーパー本文 (議論進行で更新されていく)。
    const auto paper_md = get_paper_body_by_session(session_id);
    j["paperMd"] = paper_md ? Json(*paper_md) : Json(nullptr);
    // 未来予測アドバイザリーの状態 (タグなし / 再起動後は null)。
    j["foresight"] = foresight ? to_json(*foresight) : Json(nullptr);
    j["done"] = done;
    return http::Response::json(j);
}

} // namespace

std::vector<LearningOpinion> normalize_synthetic_learning_opinions(const Json& raw_items, double posted_at) {
    std::vector<LearningOpinion> out;
    std::size_t i = 0;
    for (auto& c : select_diverse_synthetic(raw_items)) {
        LearningOpinion o;
        o.content = std::move(c.content);
        o.postedAt = posted_at + static_cast<double>(i++);
        out.push_back(std::move(o));
    }
    return out;
}

void set_flow_web_deps(FlowWebDeps deps) {
    auto p = std::make_shared<const FlowWebDeps>(std::move(deps));
    std::lock_guard lock(state().m);
    state().deps = std::move(p);
}

http::Router flow_routes() {
    http::Router r;
    r.get("/flow", [](http::Request&) { return http::Response::html(std::string(flow_html())); });
    r.post("/api/flow/start", handle_start, {kFlowStartMaxBody});
    r.get("/api/flow/:session/paper", handle_get_paper);
    r.post("/api/flow/:session/paper/form/apply", handle_form_apply, {kFlowEditMaxBody});
    r.post("/api/flow/:session/paper/debatability/check", handle_debatability_check, {kFlowSmallMaxBody});
    r.post("/api/flow/:session/paper/mechanics/check", handle_mechanics_check, {kFlowSmallMaxBody});
    r.post("/api/flow/:session/paper/fix-suggestion/apply", handle_fix_apply, {kFlowEditMaxBody});
    /** 旧「全体調整」は任意指示を LLM に渡すため廃止。チェック系の固定パターンだけを使う。 */
    r.post(
        "/api/flow/:session/paper/edit",
        [](http::Request&) {
            return json_error("全体調整は廃止しました。議論可能か確認する、メカニクス知識を確認、参考情報の取得を使ってください。", 410);
        },
        {kFlowSmallMaxBody});
    r.post("/api/flow/:session/paper/block/review", handle_block_review, {kFlowSmallMaxBody});
    r.post("/api/flow/:session/paper/block/apply", handle_block_apply, {kFlowEditMaxBody});
    r.post("/api/flow/:session/paper/crawl", handle_crawl, {kFlowSmallMaxBody});
    r.post("/api/flow/:session/paper/revert", handle_revert, {kFlowSmallMaxBody});
    r.post("/api/flow/:session/paper/approve", handle_approve, {kFlowApproveMaxBody});
    r.post("/api/flow/:session/say", handle_say, {kFlowSmallMaxBody});
    r.get("/api/flow/sessions", handle_sessions);
    r.del("/api/flow/:session", handle_delete, {kFlowSmallMaxBody});
    r.get("/api/flow/:session/status", handle_status);
    return r;
}

void reset_flow_web_for_test() {
    auto& st = state();
    std::lock_guard lock(st.m);
    st.sparring.clear();
    st.finished.clear();
    for (auto& [id, r] : st.reviews)
        if (r->timer) global_scheduler().cancel(*r->timer);
    st.reviews.clear();
    st.voiceLookups.clear();
    for (auto& [id, s] : st.running) s->request_stop();
    st.running.clear();
}

bool wait_flow_web_idle_for_test(std::chrono::milliseconds timeout) {
    auto& st = state();
    std::unique_lock lock(st.m);
    return st.idle_cv.wait_for(lock, timeout, [&] { return st.inflight == 0; });
}

} // namespace di::flow::web
