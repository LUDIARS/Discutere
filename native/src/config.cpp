#include "config.hpp"

#include <cmath>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <unordered_set>

#include "base/env.hpp"
#include "base/fsutil.hpp"
#include "base/log.hpp"
#include "base/strings.hpp"
#include "lib/one_shot/one_shot.hpp"

namespace di::config {

namespace {

using Env = std::map<std::string, std::string>;
using OptStr = std::optional<std::string>;

/** config ファイルの JSON 上限 (lorica で課金)。 */
const JsonLimits kConfigJsonLimits{kMaxConfigFileBytes, 200'000, 64};
/** env の投票レンズ JSON の上限。 */
const JsonLimits kVoteLensEnvLimits{64 * 1024, 10'000, 16};

/** path.resolve(cwd, p) */
std::string resolve_from(const std::string& cwd, std::string_view p) {
    std::filesystem::path base = fs::to_path(cwd);
    std::filesystem::path rel = fs::to_path(p);
    std::filesystem::path joined = rel.is_absolute() ? rel : base / rel;
    std::string out = fs::from_path(joined.lexically_normal());
    while (out.size() > 1 && (out.back() == '/' || out.back() == '\\')) {
#ifdef _WIN32
        if (out.size() == 3 && out[1] == ':') break;  // "C:\"
#endif
        out.pop_back();
    }
    return out;
}

/** JS の toLowerCase のうち、比較先 (ASCII の backend 名) に効く部分 (Kelvin 記号 → k を含む)。 */
std::string lower_for_match(std::string_view s) {
    return str::replace_all(str::to_lower(s), "\xE2\x84\xAA", "k");
}

struct Loader {
    const Env& env;
    const Json& file;
    std::string cwd;

    /** process.env[name] (未定義は nullopt、空文字は "" のまま)。 */
    OptStr e(const std::string& name) const {
        auto it = env.find(name);
        if (it == env.end()) return std::nullopt;
        return it->second;
    }

    /** file?.a?.b?.c — 途中が object でない / 値が null なら nullptr。 */
    const Json* f(std::initializer_list<const char*> path) const {
        const Json* cur = &file;
        for (const char* key : path) {
            if (!cur->is_object()) return nullptr;
            auto it = cur->find(key);
            if (it == cur->end()) return nullptr;
            cur = &*it;
        }
        if (cur->is_null()) return nullptr;
        return cur;
    }

    /** a ?? b (env) */
    static OptStr coalesce(const OptStr& a, const OptStr& b) { return a ? a : b; }

    /** env 優先 → file → default の順で最初に「定義された」値を採る (string) */
    static std::string pick(const OptStr& env_value, const Json* file_value, const std::string& dflt) {
        if (env_value && !env_value->empty()) return *env_value;
        if (file_value && file_value->is_string() && !file_value->get_ref<const std::string&>().empty())
            return file_value->get<std::string>();
        return dflt;
    }

    /** 同上 (optional: 定義が無ければ undefined) */
    static OptStr pick_opt(const OptStr& env_value, const Json* file_value) {
        if (env_value && !env_value->empty()) return env_value;
        if (file_value && file_value->is_string() && !file_value->get_ref<const std::string&>().empty())
            return file_value->get<std::string>();
        return std::nullopt;
    }

    /** boolean 版。env(/^(1|true|yes|on)$/i → true) → file(boolean) → default */
    static bool pick_bool(const OptStr& env_value, const Json* file_value, bool dflt) {
        if (env_value && !env_value->empty()) {
            const std::string t = str::to_lower_ascii(str::trim(*env_value));
            return t == "1" || t == "true" || t == "yes" || t == "on";
        }
        if (file_value && file_value->is_boolean()) return file_value->get<bool>();
        return dflt;
    }

    static std::optional<double> finite_number(const Json& v) {
        if (!v.is_number()) return std::nullopt;
        const double d = v.get<double>();
        if (!std::isfinite(d)) return std::nullopt;
        return d;
    }

    /** number 版。env(string)→file(number|string)→default */
    static double pick_num(const OptStr& env_value, const Json* file_value, double dflt) {
        if (env_value && !env_value->empty()) {
            if (auto n = str::js_number(*env_value); n && std::isfinite(*n)) return *n;
        }
        if (file_value) {
            if (auto n = finite_number(*file_value)) return *n;
            if (file_value->is_string() && !file_value->get_ref<const std::string&>().empty()) {
                if (auto n = str::js_number(file_value->get_ref<const std::string&>()); n && std::isfinite(*n)) return *n;
            }
        }
        return dflt;
    }

    /** 正の整数が必須な設定。指定値が不正なら既定値へ黙って戻さず fail-fast する。 */
    static double pick_positive_integer(const OptStr& env_value, const Json* file_value, double dflt,
                                        const std::string& name) {
        double value = std::nan("");
        if (env_value && !env_value->empty()) {
            value = str::js_number(*env_value).value_or(std::nan(""));
        } else if (!file_value) {
            value = dflt;
        } else if (file_value->is_number()) {
            value = file_value->get<double>();
        } else if (file_value->is_string()) {
            value = str::js_number(file_value->get_ref<const std::string&>()).value_or(std::nan(""));
        }
        if (!std::isfinite(value) || std::trunc(value) != value || value <= 0)
            throw std::runtime_error(name + " must be a positive integer");
        return value;
    }

    static std::vector<std::string> string_array(const Json* file_value) {
        std::vector<std::string> out;
        if (!file_value || !file_value->is_array()) return out;
        for (const auto& s : *file_value) {
            if (!s.is_string()) continue;
            std::string t = str::trim(s.get_ref<const std::string&>());
            if (!t.empty()) out.push_back(std::move(t));
        }
        return out;
    }

    static std::vector<std::string> split_csv(std::string_view s) {
        std::vector<std::string> out;
        for (auto& part : str::split(s, ",")) {
            std::string t = str::trim(part);
            if (!t.empty()) out.push_back(std::move(t));
        }
        return out;
    }

    /** カンマ区切り env → 無ければ file の string 配列 → 無ければ空。 */
    static std::vector<std::string> parse_string_list(const OptStr& env_value, const Json* file_value) {
        auto from_env = split_csv(env_value.value_or(""));
        if (!from_env.empty()) return from_env;
        return string_array(file_value);
    }

    /** env (カンマ区切り) → file 配列 の順で読む文字列リスト (env が空白以外を含めば env を採る)。 */
    static std::vector<std::string> parse_csv_list(const OptStr& env_value, const Json* file_value) {
        if (env_value && !str::trim(*env_value).empty()) return split_csv(*env_value);
        return string_array(file_value);
    }

    static std::vector<flow::VoteLens> coerce_lenses(const Json& raw) {
        std::vector<flow::VoteLens> out;
        if (!raw.is_array()) return out;
        for (const auto& l : raw) {
            if (!l.is_object()) continue;
            auto k = l.find("key");
            auto i = l.find("instruction");
            if (k == l.end() || i == l.end() || !k->is_string() || !i->is_string()) continue;
            if (str::trim(k->get_ref<const std::string&>()).empty()) continue;
            if (str::trim(i->get_ref<const std::string&>()).empty()) continue;
            out.push_back({k->get<std::string>(), i->get<std::string>()});
        }
        return out;
    }

    /** 投票レンズ。env は JSON 配列文字列、file は {key, instruction}[]。有効要素 0 なら既定 3 種。 */
    static std::vector<flow::VoteLens> parse_vote_lenses(const OptStr& env_value, const Json* file_value) {
        if (env_value && !str::trim(*env_value).empty()) {
            std::string error;
            if (auto parsed = parse_json(*env_value, kVoteLensEnvLimits, &error)) {
                auto from_env = coerce_lenses(*parsed);
                if (!from_env.empty()) return from_env;
            } else {
                log::warn("[config] DISCUTERE_FLOW_VOTE_LENSES の JSON パースに失敗: " + error + " — 既定レンズを使用");
            }
        }
        if (file_value) {
            auto from_file = coerce_lenses(*file_value);
            if (!from_file.empty()) return from_file;
        }
        return flow::DEFAULT_VOTE_LENSES();
    }

    /** 複数 guildIds と後方互換の単数 guildId を結合・重複排除する */
    static std::vector<std::string> merge_guild_ids(const std::vector<std::string>& list, const OptStr& single) {
        std::vector<std::string> out;
        std::unordered_set<std::string> seen;
        for (const auto& s : list)
            if (seen.insert(s).second) out.push_back(s);
        if (single && !single->empty() && seen.insert(*single).second) out.push_back(*single);
        return out;
    }

    std::string resolve(std::string_view p) const { return resolve_from(cwd, p); }
};

Json read_file_config(const Env& env, const std::string& cwd) {
    const std::string file = env.count("DISCUTERE_CONFIG") ? env.at("DISCUTERE_CONFIG") : "./discutere.config.json";
    const std::string abs = resolve_from(cwd, file);
    if (!fs::exists(abs)) return Json::object();
    auto text = fs::read_file(abs, kMaxConfigFileBytes);
    if (!text) {
        log::warn("[config] failed to read " + abs + " (unreadable or larger than limit) — ignoring file");
        return Json::object();
    }
    std::string error;
    auto parsed = parse_json(*text, kConfigJsonLimits, &error);
    if (!parsed) {
        log::warn("[config] failed to parse " + abs + ": " + error + " — ignoring file");
        return Json::object();
    }
    return parsed->is_object() ? std::move(*parsed) : Json::object();
}

std::vector<WorkerPoolWorker> workers_from(const Json* v) {
    std::vector<WorkerPoolWorker> out;
    if (!v || !v->is_array()) return out;
    for (const auto& w : *v) {
        if (!w.is_object()) {
            out.push_back({});
            continue;
        }
        out.push_back({json_string(w, "id"), json_string(w, "role"), json_string(w, "provider"), json_string(w, "model")});
    }
    return out;
}

using L = Loader;

} // namespace

DiscutereConfig load_config(const LoadOptions& options) {
    const Env env = options.env ? *options.env : env::snapshot();
    const std::string cwd = options.cwd.empty() ? env::cwd() : options.cwd;
    const Json file = read_file_config(env, cwd);
    const Loader x{env, file, cwd};
    auto e = [&](const char* name) { return x.e(name); };

    DiscutereConfig c;

    const std::string backend_raw = lower_for_match(L::pick(e("LLM_BACKEND"), x.f({"llm", "backend"}), "anthropic"));
    c.llm.backend = (backend_raw == "claude-cli" || backend_raw == "mock" || backend_raw == "worker-pool" || backend_raw == "local")
                        ? backend_raw
                        : "anthropic";

    // 分類器 backend: 既定は Lictor 経由 (claude-cli)。ANTHROPIC_API_KEY のみ存在する
    // 環境では API 直 (anthropic) に倒す (CLI が無い CI/headless での後方互換)。
    const bool has_api_key = e("ANTHROPIC_API_KEY").value_or("") != "";
    const std::string classifier_raw = lower_for_match(
        L::pick(e("DISCUTERE_CLASSIFIER_BACKEND"), x.f({"classifier", "backend"}), has_api_key ? "anthropic" : "claude-cli"));
    c.classifier.backend = (classifier_raw == "anthropic" || classifier_raw == "off" || classifier_raw == "local")
                               ? classifier_raw
                               : "claude-cli";

    c.nodeEnv = L::pick(e("NODE_ENV"), nullptr, "development");
    c.externalDiscussion.sharedSecret = e("DISCUTERE_EXTERNAL_DISCUSSION_SECRET").value_or("");
    c.glabConclusions.token = e("DISCUTERE_GLAB_CONCLUSIONS_TOKEN").value_or("");

    c.server.port = L::pick_num(e("BACKEND_PORT"), x.f({"server", "port"}), 3110);
    c.server.host = L::pick(e("BACKEND_HOST"), x.f({"server", "host"}), "127.0.0.1");
    c.server.frontendUrl = L::pick(e("FRONTEND_URL"), x.f({"server", "frontendUrl"}), "http://localhost:5174");

    // ─── flow ───
    auto& fl = c.flow;
    fl.rounds = L::pick_num(e("DISCUTERE_FLOW_ROUNDS"), x.f({"flow", "rounds"}), 3);
    fl.turnsPerRound = L::pick_num(e("DISCUTERE_FLOW_TURNS_PER_ROUND"), x.f({"flow", "turnsPerRound"}), 6);
    fl.personaCount = L::pick_num(e("DISCUTERE_FLOW_PERSONA_COUNT"), x.f({"flow", "personaCount"}), 4);
    fl.roster.facilitator = L::pick(e("DISCUTERE_FLOW_ROSTER_FACILITATOR"), x.f({"flow", "roster", "facilitator"}), "");
    fl.roster.discussants = L::parse_string_list(e("DISCUTERE_FLOW_ROSTER_DISCUSSANTS"), x.f({"flow", "roster", "discussants"}));
    fl.voterCount = L::pick_num(e("DISCUTERE_FLOW_VOTER_COUNT"), x.f({"flow", "voterCount"}), 3);
    fl.voteLenses = L::parse_vote_lenses(e("DISCUTERE_FLOW_VOTE_LENSES"), x.f({"flow", "voteLenses"}));
    fl.convergeShare = L::pick_num(e("DISCUTERE_FLOW_CONVERGE_SHARE"), x.f({"flow", "convergeShare"}), 0.6);
    // 議論エンジン (PR-B)。既定 "dialectic"。"dialectic" 以外の明示値 (rounds / 不正値) は "rounds" に倒す。
    fl.engine = L::pick(e("DISCUTERE_FLOW_ENGINE"), x.f({"flow", "engine"}), "dialectic") == "dialectic" ? "dialectic" : "rounds";
    fl.dialectic.rebutTurns = L::pick_num(e("DISCUTERE_FLOW_DIALECTIC_REBUT_TURNS"), x.f({"flow", "dialectic", "rebutTurns"}), 4);
    fl.dialectic.exchangeTurns =
        L::pick_num(e("DISCUTERE_FLOW_DIALECTIC_EXCHANGE_TURNS"), x.f({"flow", "dialectic", "exchangeTurns"}), 16);
    fl.dialectic.argumentDepth =
        L::pick_num(e("DISCUTERE_FLOW_DIALECTIC_ARGUMENT_DEPTH"), x.f({"flow", "dialectic", "argumentDepth"}), 4);
    fl.dialectic.staleTurns = L::pick_num(e("DISCUTERE_FLOW_DIALECTIC_STALE_TURNS"), x.f({"flow", "dialectic", "staleTurns"}), 6);
    fl.dialectic.judgeModel = L::pick(e("DISCUTERE_FLOW_DIALECTIC_JUDGE_MODEL"), x.f({"flow", "dialectic", "judgeModel"}),
                                      "claude-haiku-4-5-20251001");
    fl.dialectic.ruleCheck = L::pick_bool(e("DISCUTERE_FLOW_DIALECTIC_RULE_CHECK"), x.f({"flow", "dialectic", "ruleCheck"}), true);
    fl.dialectic.finalJudgeModel =
        L::pick(e("DISCUTERE_FLOW_DIALECTIC_FINAL_JUDGE_MODEL"), x.f({"flow", "dialectic", "finalJudgeModel"}), "");
    fl.personaSetupModel = L::pick(e("DISCUTERE_FLOW_PERSONA_SETUP_MODEL"), x.f({"flow", "personaSetupModel"}), "");
    fl.tags = L::parse_string_list(e("DISCUTERE_FLOW_TAGS"), x.f({"flow", "tags"}));
    fl.sparringMaxTurns = L::pick_num(e("DISCUTERE_FLOW_SPARRING_MAX_TURNS"), x.f({"flow", "sparringMaxTurns"}), 50);
    fl.youtubeMaxComments = L::pick_num(e("DISCUTERE_FLOW_YOUTUBE_MAX_COMMENTS"), x.f({"flow", "youtubeMaxComments"}), 200);
    fl.autoAdoptOnCrawl = L::pick_bool(e("DISCUTERE_FLOW_AUTO_ADOPT_ON_CRAWL"), x.f({"flow", "autoAdoptOnCrawl"}), false);

    fl.autoCrawl.enabled = L::pick_bool(e("DISCUTERE_FLOW_AUTOCRAWL_ENABLED"), x.f({"flow", "autoCrawl", "enabled"}), true);
    fl.autoCrawl.source = L::pick(e("DISCUTERE_FLOW_AUTOCRAWL_SOURCE"), x.f({"flow", "autoCrawl", "source"}), "niconico");
    fl.autoCrawl.sources = L::parse_string_list(e("DISCUTERE_FLOW_AUTOCRAWL_SOURCES"), x.f({"flow", "autoCrawl", "sources"}));
    // 後方互換: sources 未指定なら単数 source を 1 要素として使う。
    if (fl.autoCrawl.sources.empty()) fl.autoCrawl.sources = {fl.autoCrawl.source};
    fl.autoCrawl.minVoices = L::pick_num(e("DISCUTERE_FLOW_AUTOCRAWL_MIN_VOICES"), x.f({"flow", "autoCrawl", "minVoices"}), 3);
    fl.autoCrawl.maxItems = L::pick_num(e("DISCUTERE_FLOW_AUTOCRAWL_MAX_ITEMS"), x.f({"flow", "autoCrawl", "maxItems"}), 300);

    fl.informationGating.enabled =
        L::pick_bool(e("DISCUTERE_FLOW_INFOGATE_ENABLED"), x.f({"flow", "informationGating", "enabled"}), true);
    fl.informationGating.minDensity =
        L::pick(e("DISCUTERE_FLOW_INFOGATE_MIN_DENSITY"), x.f({"flow", "informationGating", "minDensity"}), "moderate");
    fl.informationGating.maxLearnIterations =
        L::pick_num(e("DISCUTERE_FLOW_INFOGATE_MAX_ITERATIONS"), x.f({"flow", "informationGating", "maxLearnIterations"}), 2);
    fl.informationGating.maxGapsPerIteration =
        L::pick_num(e("DISCUTERE_FLOW_INFOGATE_MAX_GAPS"), x.f({"flow", "informationGating", "maxGapsPerIteration"}), 2);
    fl.informationGating.model = L::pick(e("DISCUTERE_FLOW_INFOGATE_MODEL"), x.f({"flow", "informationGating", "model"}), "");

    fl.debatability.enabled = L::pick_bool(e("DISCUTERE_FLOW_DEBATABILITY_ENABLED"), x.f({"flow", "debatability", "enabled"}), true);
    fl.debatability.minArmableIssues = L::pick_num(e("DISCUTERE_FLOW_DEBATABILITY_MIN_ARMABLE_ISSUES"),
                                                   x.f({"flow", "debatability", "minArmableIssues"}), 2);

    fl.paperReview.enabled = L::pick_bool(e("DISCUTERE_FLOW_PAPER_REVIEW_ENABLED"), x.f({"flow", "paperReview", "enabled"}), false);
    fl.paperReview.webCanonical =
        L::pick_bool(e("DISCUTERE_FLOW_PAPER_REVIEW_WEB_CANONICAL"), x.f({"flow", "paperReview", "webCanonical"}), true);
    fl.paperReview.model = L::pick(e("DISCUTERE_FLOW_PAPER_REVIEW_MODEL"), x.f({"flow", "paperReview", "model"}), "");
    fl.paperReview.timeoutMs = L::pick_num(e("DISCUTERE_FLOW_PAPER_REVIEW_TIMEOUT_MS"), x.f({"flow", "paperReview", "timeoutMs"}), 0);

    fl.paperRichness.voices = L::pick_num(e("DISCUTERE_FLOW_PAPER_VOICES"), x.f({"flow", "paperRichness", "voices"}), 15);
    fl.paperRichness.mechanicsTarget =
        L::pick_num(e("DISCUTERE_FLOW_PAPER_MECHANICS_TARGET"), x.f({"flow", "paperRichness", "mechanicsTarget"}), 30);
    fl.paperRichness.enrichMechanics =
        L::pick_bool(e("DISCUTERE_FLOW_PAPER_ENRICH_MECHANICS"), x.f({"flow", "paperRichness", "enrichMechanics"}), true);
    fl.paperRichness.enrichModel = L::pick(e("DISCUTERE_FLOW_PAPER_ENRICH_MODEL"), x.f({"flow", "paperRichness", "enrichModel"}), "");

    fl.userVoices.enabled = L::pick_bool(e("DISCUTERE_FLOW_USER_VOICES_ENABLED"), x.f({"flow", "userVoices", "enabled"}), true);
    fl.userVoices.steamMaxReviews =
        L::pick_num(e("DISCUTERE_FLOW_USER_VOICES_STEAM_MAX"), x.f({"flow", "userVoices", "steamMaxReviews"}), 300);
    {
        // file?.steamLanguages ?? ["japanese", "english"]
        static const Json default_languages = Json::array({"japanese", "english"});
        const Json* langs = x.f({"flow", "userVoices", "steamLanguages"});
        fl.userVoices.steamLanguages =
            L::parse_string_list(e("DISCUTERE_FLOW_USER_VOICES_STEAM_LANGUAGES"), langs ? langs : &default_languages);
    }
    fl.userVoices.glabMaxImpressions =
        L::pick_num(e("DISCUTERE_FLOW_USER_VOICES_GLAB_MAX"), x.f({"flow", "userVoices", "glabMaxImpressions"}), 100);
    // 明示指定 > config ファイル > Excubitor が全サービスへ配る topology env (VOLUPTAS_URL)。
    // Excubitor のサービスコードが旧綴り volputas の間は VOLPUTAS_URL で届くので、それも読む。
    fl.userVoices.voluptasBaseUrl = L::pick(e("DISCUTERE_VOLUPTAS_BASE_URL"), x.f({"flow", "userVoices", "voluptasBaseUrl"}),
                                            L::coalesce(e("VOLUPTAS_URL"), e("VOLPUTAS_URL")).value_or(""));

    fl.notionLinks.maxDepth = L::pick_num(e("DISCUTERE_NOTION_MAX_DEPTH"), x.f({"flow", "notionLinks", "maxDepth"}), 2);
    fl.notionLinks.maxPages = L::pick_num(e("DISCUTERE_NOTION_MAX_PAGES"), x.f({"flow", "notionLinks", "maxPages"}), 50);
    fl.improvement.effectModel = L::pick(e("DISCUTERE_FLOW_IMPROVEMENT_EFFECT_MODEL"), x.f({"flow", "improvement", "effectModel"}),
                                         "claude-haiku-4-5-20251001");
    fl.paperRefine.enabled = L::pick_bool(e("DISCUTERE_FLOW_PAPER_REFINE_ENABLED"), x.f({"flow", "paperRefine", "enabled"}), false);
    fl.paperRefine.model = L::pick(e("DISCUTERE_FLOW_PAPER_REFINE_MODEL"), x.f({"flow", "paperRefine", "model"}), "");

    fl.foresight.enabled = L::pick_bool(e("DISCUTERE_FLOW_FORESIGHT_ENABLED"), x.f({"flow", "foresight", "enabled"}), true);
    fl.foresight.turns = std::max(
        1.0, std::min(20.0, std::floor(L::pick_num(e("DISCUTERE_FLOW_FORESIGHT_TURNS"), x.f({"flow", "foresight", "turns"}), 6))));
    fl.foresight.model = L::pick(e("DISCUTERE_FLOW_FORESIGHT_MODEL"), x.f({"flow", "foresight", "model"}), "");
    fl.foresight.forecastModel =
        L::pick(e("DISCUTERE_FLOW_FORESIGHT_FORECAST_MODEL"), x.f({"flow", "foresight", "forecastModel"}), "");

    {
        const double max_budget = std::max(
            500.0, std::min(10000.0, std::floor(L::pick_num(e("DISCUTERE_FLOW_PROXY_MAX_BUDGET_TOKENS"),
                                                           x.f({"flow", "proxy", "maxBudgetTokens"}), 10000))));
        fl.proxy.enabled = L::pick_bool(e("DISCUTERE_FLOW_PROXY_ENABLED"), x.f({"flow", "proxy", "enabled"}), false);
        fl.proxy.budgetTokens = std::max(
            500.0, std::min(max_budget, std::floor(L::pick_num(e("DISCUTERE_FLOW_PROXY_BUDGET_TOKENS"),
                                                              x.f({"flow", "proxy", "budgetTokens"}), 3500))));
        fl.proxy.maxBudgetTokens = max_budget;
        fl.proxy.stanceModel = L::pick(e("DISCUTERE_FLOW_PROXY_STANCE_MODEL"), x.f({"flow", "proxy", "stanceModel"}), "");
        fl.proxy.requireEvalPassed =
            L::pick_bool(e("DISCUTERE_FLOW_PROXY_REQUIRE_EVAL_PASSED"), x.f({"flow", "proxy", "requireEvalPassed"}), true);
    }

    fl.anatomia.enabled = L::pick_bool(e("DISCUTERE_ANATOMIA_ENABLED"), x.f({"flow", "anatomia", "enabled"}), false);
    fl.anatomia.binPath = L::pick(e("DISCUTERE_ANATOMIA_BIN"), x.f({"flow", "anatomia", "binPath"}), "");
    fl.anatomia.autoDraft = L::pick_bool(e("DISCUTERE_ANATOMIA_AUTO_DRAFT"), x.f({"flow", "anatomia", "autoDraft"}), true);
    fl.anatomia.refineModel = L::pick(e("DISCUTERE_ANATOMIA_REFINE_MODEL"), x.f({"flow", "anatomia", "refineModel"}), "");
    fl.anatomia.timeoutMs = L::pick_num(e("DISCUTERE_ANATOMIA_TIMEOUT_MS"), x.f({"flow", "anatomia", "timeoutMs"}), 180000);

    c.workspace = L::pick(e("DISCATIER_WORKSPACE"), x.f({"workspace"}), "knowledge");

    c.discatier.kuzuPath = L::pick_opt(e("DISCATIER_KUZU_PATH"), x.f({"discatier", "kuzuPath"}));
    c.discatier.activeKg = L::pick_opt(e("DISCATIER_ACTIVE_KG"), x.f({"discatier", "activeKg"}));
    // knowledgeGraphs は config ファイルからのみ (env で配列は受けない)。不正要素は除外。
    if (const Json* kgs = x.f({"discatier", "knowledgeGraphs"}); kgs && kgs->is_array()) {
        std::vector<KnowledgeGraphEntry> list;
        for (const auto& kg : *kgs) {
            if (!kg.is_object()) continue;
            auto id = kg.find("id");
            auto path = kg.find("kuzuPath");
            if (id == kg.end() || path == kg.end() || !id->is_string() || !path->is_string()) continue;
            KnowledgeGraphEntry entry{id->get<std::string>(), std::nullopt, path->get<std::string>()};
            if (auto label = kg.find("label"); label != kg.end() && label->is_string()) entry.label = label->get<std::string>();
            list.push_back(std::move(entry));
        }
        c.discatier.knowledgeGraphs = std::move(list);
    }

    c.personaEngine.enabled = L::pick_bool(e("DISCUTERE_PERSONA_ENGINE_ENABLED"), x.f({"personaEngine", "enabled"}), false);
    c.personaEngine.dbPath =
        L::pick(e("DISCUTERE_PERSONA_ENGINE_DB"), x.f({"personaEngine", "dbPath"}), x.resolve("./data/persona-engine.db"));
    c.personaEngine.maxFiresPerSession =
        L::pick_num(e("PERSONA_ENGINE_MAX_FIRES_PER_SESSION"), x.f({"personaEngine", "maxFiresPerSession"}), 20);
    c.personaEngine.maxFiresPerRule = L::pick_num(e("PERSONA_ENGINE_MAX_FIRES_PER_RULE"), x.f({"personaEngine", "maxFiresPerRule"}), 5);
    c.personaEngine.tickMs = L::pick_num(e("PERSONA_ENGINE_TICK_MS"), x.f({"personaEngine", "tickMs"}), 5000);
    c.personaEngine.bridgePollMs = L::pick_num(e("PERSONA_ENGINE_BRIDGE_POLL_MS"), x.f({"personaEngine", "bridgePollMs"}), 2000);

    c.facilitator.enabled = L::pick_bool(e("DISCUTERE_FACILITATOR_ENABLED"), x.f({"facilitator", "enabled"}), true);
    c.facilitator.tickMs = L::pick_num(e("DISCUTERE_FACILITATOR_TICK_MS"), x.f({"facilitator", "tickMs"}), 30'000);
    c.facilitator.idleGapMs = L::pick_num(e("DISCUTERE_FACILITATOR_IDLE_GAP_MS"), x.f({"facilitator", "idleGapMs"}), 120'000);
    c.facilitator.maxPersonas = L::pick_num(e("DISCUTERE_FACILITATOR_MAX_PERSONAS"), x.f({"facilitator", "maxPersonas"}), 20);
    c.facilitator.aufhebungTarget =
        L::pick_num(e("DISCUTERE_FACILITATOR_AUFHEBUNG_TARGET"), x.f({"facilitator", "aufhebungTarget"}), 3);

    c.autoSeed.enabled = L::pick_bool(e("DISCUTERE_AUTOSEED_ENABLED"), x.f({"autoSeed", "enabled"}), false);
    c.autoSeed.intervalMs = L::pick_num(e("DISCUTERE_AUTOSEED_INTERVAL_MS"), x.f({"autoSeed", "intervalMs"}), 1'800'000);
    c.autoSeed.maxConcurrent = L::pick_num(e("DISCUTERE_AUTOSEED_MAX_CONCURRENT"), x.f({"autoSeed", "maxConcurrent"}), 2);
    c.autoSeed.sources = L::parse_string_list(e("DISCUTERE_AUTOSEED_SOURCES"), x.f({"autoSeed", "sources"}));
    if (c.autoSeed.sources.empty()) c.autoSeed.sources = {"genre"};

    c.llm.anthropicApiKey = L::pick_opt(e("ANTHROPIC_API_KEY"), x.f({"llm", "anthropicApiKey"}));
    c.llm.model = L::pick_opt(e("ANTHROPIC_MODEL"), x.f({"llm", "model"}));
    c.llm.tiers.cheap = L::pick_opt(e("LLM_TIER_CHEAP"), x.f({"llm", "tiers", "cheap"}));
    c.llm.tiers.strong = L::pick_opt(e("LLM_TIER_STRONG"), x.f({"llm", "tiers", "strong"}));
    c.llm.claudeCliTimeoutMs = L::pick_num(e("CLAUDE_CLI_TIMEOUT_MS"), x.f({"llm", "claudeCliTimeoutMs"}), 120'000);
    c.llm.gitBashPath = L::pick_opt(e("CLAUDE_CODE_GIT_BASH_PATH"), x.f({"llm", "gitBashPath"}));
    // 既定は Ollama の OpenAI 互換エンドポイント。vLLM/LM Studio は baseUrl を差し替え。
    c.llm.local.baseUrl = L::pick(e("LLM_LOCAL_BASE_URL"), x.f({"llm", "local", "baseUrl"}), "http://localhost:11434/v1");
    c.llm.local.model = L::pick(e("LLM_LOCAL_MODEL"), x.f({"llm", "local", "model"}), "gemma4:12b");
    c.llm.local.apiKey = L::pick_opt(e("LLM_LOCAL_API_KEY"), x.f({"llm", "local", "apiKey"}));
    c.llm.local.timeoutMs = L::pick_num(e("LLM_LOCAL_TIMEOUT_MS"), x.f({"llm", "local", "timeoutMs"}), 120'000);

    c.embedding.enabled = L::pick_bool(e("DISCUTERE_EMBEDDING_ENABLED"), x.f({"embedding", "enabled"}), false);
    c.embedding.baseUrl = L::pick(e("DISCUTERE_EMBEDDING_BASE_URL"), x.f({"embedding", "baseUrl"}), "http://localhost:11434/v1");
    c.embedding.model = L::pick(e("DISCUTERE_EMBEDDING_MODEL"), x.f({"embedding", "model"}), "bge-m3");
    c.embedding.apiKey = L::pick_opt(e("DISCUTERE_EMBEDDING_API_KEY"), x.f({"embedding", "apiKey"}));
    // CPU 埋め込み (bge-m3) はバッチ処理中の一過性遅延で 30s を超えることがあった。既定は余裕を持たせる。
    c.embedding.timeoutMs = L::pick_positive_integer(e("DISCUTERE_EMBEDDING_TIMEOUT_MS"), x.f({"embedding", "timeoutMs"}), 60'000,
                                                     "embedding.timeoutMs");
    c.embedding.batchSize =
        L::pick_positive_integer(e("DISCUTERE_EMBEDDING_BATCH_SIZE"), x.f({"embedding", "batchSize"}), 32, "embedding.batchSize");

    c.classifier.model = L::pick(e("DISCUTERE_CLASSIFIER_MODEL"), x.f({"classifier", "model"}), "claude-haiku-4-5-20251001");
    c.classifier.timeoutMs = L::pick_num(e("DISCUTERE_CLASSIFIER_TIMEOUT_MS"), x.f({"classifier", "timeoutMs"}), 30'000);

    // TS と同じく既定値のモデル解決は常に評価する (env の上書きが不正なら起動時に失敗する)。
    const std::string opus = lib::one_shot::resolve_model(std::string("opus"), "claude", env);
    c.discussion.expectedUtterances = L::pick_num(e("DISCUTERE_DISCUSSION_EXPECTED"), x.f({"discussion", "expectedUtterances"}), 20);
    c.discussion.keymanCount = L::pick_num(e("DISCUTERE_DISCUSSION_KEYMEN"), x.f({"discussion", "keymanCount"}), 2);
    c.discussion.facilitatorModel = L::pick(e("DISCUTERE_DISCUSSION_FACILITATOR_MODEL"), x.f({"discussion", "facilitatorModel"}), opus);
    c.discussion.keymanModel = L::pick(e("DISCUTERE_DISCUSSION_KEYMAN_MODEL"), x.f({"discussion", "keymanModel"}),
                                       lib::one_shot::resolve_model(std::string("opus"), "claude", env));
    if (const Json* om = x.f({"discussion", "opinionModels"}); om && om->is_array() && !om->empty()) {
        for (const auto& m : *om) {
            if (!m.is_object()) {
                c.discussion.opinionModels.push_back({});
                continue;
            }
            c.discussion.opinionModels.push_back({json_string(m, "model"), json_number(m, "weight")});
        }
    } else {
        c.discussion.opinionModels = {
            {lib::one_shot::resolve_model(std::string("sonnet"), "claude", env), 6},
            {lib::one_shot::resolve_model(std::string("haiku"), "claude", env), 4},
        };
    }
    c.discussion.minTotal = L::pick_num(e("DISCUTERE_DISCUSSION_MIN_TOTAL"), x.f({"discussion", "minTotal"}), 4);
    c.discussion.turnDelayMs = L::pick_num(e("DISCUTERE_DISCUSSION_TURN_DELAY_MS"), x.f({"discussion", "turnDelayMs"}), 4'000);
    c.discussion.continueTimeoutMs =
        L::pick_num(e("DISCUTERE_DISCUSSION_CONTINUE_TIMEOUT_MS"), x.f({"discussion", "continueTimeoutMs"}), 120'000);
    c.discussion.maxRounds = L::pick_num(e("DISCUTERE_DISCUSSION_MAX_ROUNDS"), x.f({"discussion", "maxRounds"}), 5);

    auto& wp = c.workerPool;
    wp.enabled = L::pick_bool(e("DISCUTERE_WORKER_POOL_ENABLED"), x.f({"workerPool", "enabled"}), false);
    wp.workspace = L::pick(e("DISCUTERE_WORKER_POOL_WORKSPACE"), x.f({"workerPool", "workspace"}), "debate");
    wp.callbackBaseUrl = L::pick(e("DISCUTERE_WORKER_POOL_CALLBACK_URL"), x.f({"workerPool", "callbackBaseUrl"}),
                                 "http://127.0.0.1:" + js_number_to_string(L::pick_num(e("BACKEND_PORT"), x.f({"server", "port"}), 3110)));
    {
        // file.workerPool?.gitBashPath ?? file.llm?.gitBashPath
        const Json* fg = x.f({"workerPool", "gitBashPath"});
        if (!fg) fg = x.f({"llm", "gitBashPath"});
        wp.gitBashPath = L::pick_opt(L::coalesce(e("DISCUTERE_WORKER_POOL_GIT_BASH"), e("CLAUDE_CODE_GIT_BASH_PATH")), fg);
    }
    wp.injectDelayMs = L::pick_num(e("DISCUTERE_WORKER_POOL_INJECT_DELAY_MS"), x.f({"workerPool", "injectDelayMs"}), 2500);
    wp.turnTimeoutMs = L::pick_num(e("DISCUTERE_WORKER_POOL_TURN_TIMEOUT_MS"), x.f({"workerPool", "turnTimeoutMs"}), 120'000);
    wp.registerTimeoutMs =
        L::pick_num(e("DISCUTERE_WORKER_POOL_REGISTER_TIMEOUT_MS"), x.f({"workerPool", "registerTimeoutMs"}), 60'000);
    wp.registerSettleMs = L::pick_num(e("DISCUTERE_WORKER_POOL_REGISTER_SETTLE_MS"), x.f({"workerPool", "registerSettleMs"}), 4'000);
    wp.workers = workers_from(x.f({"workerPool", "workers"}));
    wp.excludeProviders = L::parse_csv_list(e("DISCUTERE_WORKER_POOL_EXCLUDE_PROVIDERS"), x.f({"workerPool", "excludeProviders"}));

    auto& dc = c.discord;
    dc.botToken = L::pick_opt(e("DISCUTERE_DISCORD_BOT_TOKEN"), x.f({"discord", "botToken"}));
    dc.applicationId = L::pick_opt(e("DISCUTERE_DISCORD_APPLICATION_ID"), x.f({"discord", "applicationId"}));
    // guildIds: env (カンマ区切り) → file.guildIds → 後方互換で単数 guildId を1要素に。
    dc.guildIds = L::merge_guild_ids(L::parse_string_list(e("DISCUTERE_DISCORD_GUILD_IDS"), x.f({"discord", "guildIds"})),
                                     L::pick_opt(e("DISCUTERE_DISCORD_GUILD_ID"), x.f({"discord", "guildId"})));
    dc.adminIds = L::parse_string_list(e("DISCUTERE_DISCORD_ADMIN_IDS"), x.f({"discord", "adminIds"}));
    dc.discussionChannelIds =
        L::parse_string_list(e("DISCUTERE_DISCORD_DISCUSSION_CHANNELS"), x.f({"discord", "discussionChannelIds"}));
    dc.crawlChannelIds = L::parse_string_list(e("DISCUTERE_DISCORD_CRAWL_CHANNELS"), x.f({"discord", "crawlChannelIds"}));
    dc.forum.enabled = L::pick_bool(e("DISCUTERE_DISCORD_FORUM_ENABLED"), x.f({"discord", "forum", "enabled"}), true);
    dc.forum.summaryChannelName =
        L::pick(e("DISCUTERE_DISCORD_FORUM_SUMMARY_CHANNEL"), x.f({"discord", "forum", "summaryChannelName"}), "まとめ投稿");
    dc.forum.dataLearningChannelName =
        L::pick(e("DISCUTERE_DISCORD_FORUM_DATA_CHANNEL"), x.f({"discord", "forum", "dataLearningChannelName"}), "データ学習依頼");
    dc.forum.managedCategoryName =
        L::pick(e("DISCUTERE_DISCORD_FORUM_CATEGORY"), x.f({"discord", "forum", "managedCategoryName"}), "システム");
    dc.forum.improvementTagNames =
        L::parse_string_list(e("DISCUTERE_DISCORD_FORUM_IMPROVEMENT_TAGS"), x.f({"discord", "forum", "improvementTagNames"}));
    if (dc.forum.improvementTagNames.empty()) dc.forum.improvementTagNames = {"改善提案", "改善", "提案", "improvement"};
    dc.forum.funTagNames = L::parse_string_list(e("DISCUTERE_DISCORD_FORUM_FUN_TAGS"), x.f({"discord", "forum", "funTagNames"}));
    if (dc.forum.funTagNames.empty()) dc.forum.funTagNames = {"面白さ", "面白い", "おもしろさ", "fun"};
    dc.forum.discussionForumName =
        L::pick(e("DISCUTERE_DISCORD_FORUM_NAME"), x.f({"discord", "forum", "discussionForumName"}), "議論");
    dc.gameFeedback.enabled =
        L::pick_bool(e("DISCUTERE_DISCORD_GAME_FEEDBACK_ENABLED"), x.f({"discord", "gameFeedback", "enabled"}), true);
    dc.gameFeedback.categoryName =
        L::pick(e("DISCUTERE_DISCORD_GAME_FEEDBACK_CATEGORY"), x.f({"discord", "gameFeedback", "categoryName"}), "ゲーム感想");

    c.slack.enabled = L::pick_bool(e("DISCUTERE_SLACK_ENABLED"), x.f({"slack", "enabled"}), false);
    c.slack.appToken = L::pick_opt(e("DISCUTERE_SLACK_APP_TOKEN"), x.f({"slack", "appToken"})).value_or("");
    c.slack.botToken = L::pick_opt(e("DISCUTERE_SLACK_BOT_TOKEN"), x.f({"slack", "botToken"})).value_or("");
    c.slack.channelIds = L::parse_string_list(e("DISCUTERE_SLACK_CHANNEL_IDS"), x.f({"slack", "channelIds"}));
    c.slack.adminIds = L::parse_string_list(e("DISCUTERE_SLACK_ADMIN_IDS"), x.f({"slack", "adminIds"}));

    auto& bk = c.backup;
    bk.enabled = L::pick_bool(e("DISCUTERE_BACKUP_ENABLED"), x.f({"backup", "enabled"}), false);
    bk.bucket = L::pick_opt(e("DISCUTERE_BACKUP_BUCKET"), x.f({"backup", "bucket"}));
    bk.region = L::pick(L::coalesce(e("DISCUTERE_BACKUP_REGION"), e("AWS_REGION")), x.f({"backup", "region"}), "ap-northeast-1");
    bk.prefix = L::pick(e("DISCUTERE_BACKUP_PREFIX"), x.f({"backup", "prefix"}), "discutere/");
    bk.storageClass = L::pick(e("DISCUTERE_BACKUP_STORAGE_CLASS"), x.f({"backup", "storageClass"}), "GLACIER");
    bk.endpoint = L::pick_opt(e("DISCUTERE_BACKUP_ENDPOINT"), x.f({"backup", "endpoint"}));
    bk.accessKeyId = L::pick_opt(e("AWS_ACCESS_KEY_ID"), x.f({"backup", "accessKeyId"}));
    bk.secretAccessKey = L::pick_opt(e("AWS_SECRET_ACCESS_KEY"), x.f({"backup", "secretAccessKey"}));
    bk.intervalDays = L::pick_num(e("DISCUTERE_BACKUP_INTERVAL_DAYS"), x.f({"backup", "intervalDays"}), 30);
    bk.stateFile = L::pick(e("DISCUTERE_BACKUP_STATE_FILE"), x.f({"backup", "stateFile"}), x.resolve("./data/backup-state.json"));

    c.fundamentum.enabled = L::pick_bool(e("DISCUTERE_FUNDAMENTUM_ENABLED"), x.f({"fundamentum", "enabled"}), true);
    c.fundamentum.dataDir =
        L::pick(e("DISCUTERE_FUNDAMENTUM_DATA_DIR"), x.f({"fundamentum", "dataDir"}), x.resolve("./data/fundamentum"));

    c.kgSync.sourceUrl = L::pick_opt(e("DISCUTERE_KG_SYNC_URL"), x.f({"kgSync", "sourceUrl"}));
    c.kgSync.token = L::pick_opt(e("DISCUTERE_KG_SYNC_TOKEN"), x.f({"kgSync", "token"}));
    c.kgSync.auto_ = L::pick_bool(e("DISCUTERE_KG_SYNC_AUTO"), x.f({"kgSync", "auto"}), false);
    c.kgSync.intervalHours = L::pick_num(e("DISCUTERE_KG_SYNC_INTERVAL_HOURS"), x.f({"kgSync", "intervalHours"}), 6);
    c.kgSync.stateFile =
        L::pick(e("DISCUTERE_KG_SYNC_STATE_FILE"), x.f({"kgSync", "stateFile"}), x.resolve("./data/kg-sync-state.json"));
    c.kgSync.serveToken = L::pick_opt(e("DISCUTERE_KG_SYNC_SERVE_TOKEN"), x.f({"kgSync", "serveToken"}));

    c.cost.relay.enabled = L::pick_bool(e("DISCUTERE_COST_RELAY_ENABLED"), x.f({"cost", "relay", "enabled"}), false);
    c.cost.relay.anatomiaUrl = L::pick_opt(e("DISCUTERE_COST_RELAY_URL"), x.f({"cost", "relay", "anatomiaUrl"}));
    c.cost.relay.concordiaUrl = L::pick_opt(e("DISCUTERE_COST_RELAY_CONCORDIA_URL"), x.f({"cost", "relay", "concordiaUrl"}));
    c.cost.relay.intervalMs = L::pick_num(e("DISCUTERE_COST_RELAY_INTERVAL_MS"), x.f({"cost", "relay", "intervalMs"}), 300'000);
    c.cost.relay.service = L::pick(e("DISCUTERE_COST_RELAY_SERVICE"), x.f({"cost", "relay", "service"}), "discutere");
    return c;
}

namespace {
std::mutex g_mutex;
std::optional<DiscutereConfig> g_cached;
}  // namespace

const DiscutereConfig& get_config() {
    std::lock_guard lock(g_mutex);
    if (!g_cached) g_cached = load_config();
    return *g_cached;
}

void reset_config_for_test() {
    std::lock_guard lock(g_mutex);
    g_cached.reset();
}

// ─── JSON (JSON.stringify と同じ形: undefined のキーは出さない) ─────────────

namespace {

void put_opt(Json& o, const char* key, const std::optional<std::string>& v) {
    if (v) o[key] = *v;
}

Json strings(const std::vector<std::string>& v) {
    Json a = Json::array();
    for (const auto& s : v) a.push_back(s);
    return a;
}

}  // namespace

Json to_json(const DiscutereConfig& c) {
    Json o = Json::object();
    o["nodeEnv"] = c.nodeEnv;
    o["externalDiscussion"] = Json{{"sharedSecret", c.externalDiscussion.sharedSecret}};
    o["glabConclusions"] = Json{{"token", c.glabConclusions.token}};
    o["server"] = Json{{"port", c.server.port}, {"host", c.server.host}, {"frontendUrl", c.server.frontendUrl}};

    const auto& fl = c.flow;
    Json f = Json::object();
    f["rounds"] = fl.rounds;
    f["turnsPerRound"] = fl.turnsPerRound;
    f["personaCount"] = fl.personaCount;
    f["roster"] = Json{{"facilitator", fl.roster.facilitator}, {"discussants", strings(fl.roster.discussants)}};
    f["voterCount"] = fl.voterCount;
    Json lenses = Json::array();
    for (const auto& l : fl.voteLenses) lenses.push_back(flow::to_json(l));
    f["voteLenses"] = std::move(lenses);
    f["convergeShare"] = fl.convergeShare;
    f["engine"] = fl.engine;
    f["dialectic"] = Json{{"rebutTurns", fl.dialectic.rebutTurns},       {"exchangeTurns", fl.dialectic.exchangeTurns},
                          {"argumentDepth", fl.dialectic.argumentDepth}, {"staleTurns", fl.dialectic.staleTurns},
                          {"judgeModel", fl.dialectic.judgeModel},       {"ruleCheck", fl.dialectic.ruleCheck},
                          {"finalJudgeModel", fl.dialectic.finalJudgeModel}};
    f["personaSetupModel"] = fl.personaSetupModel;
    f["tags"] = strings(fl.tags);
    f["sparringMaxTurns"] = fl.sparringMaxTurns;
    f["youtubeMaxComments"] = fl.youtubeMaxComments;
    f["autoAdoptOnCrawl"] = fl.autoAdoptOnCrawl;
    f["autoCrawl"] = Json{{"enabled", fl.autoCrawl.enabled},
                          {"source", fl.autoCrawl.source},
                          {"sources", strings(fl.autoCrawl.sources)},
                          {"minVoices", fl.autoCrawl.minVoices},
                          {"maxItems", fl.autoCrawl.maxItems}};
    f["informationGating"] = Json{{"enabled", fl.informationGating.enabled},
                                  {"minDensity", fl.informationGating.minDensity},
                                  {"maxLearnIterations", fl.informationGating.maxLearnIterations},
                                  {"maxGapsPerIteration", fl.informationGating.maxGapsPerIteration},
                                  {"model", fl.informationGating.model}};
    f["debatability"] = Json{{"enabled", fl.debatability.enabled}, {"minArmableIssues", fl.debatability.minArmableIssues}};
    f["paperReview"] = Json{{"enabled", fl.paperReview.enabled},
                            {"webCanonical", fl.paperReview.webCanonical},
                            {"model", fl.paperReview.model},
                            {"timeoutMs", fl.paperReview.timeoutMs}};
    f["paperRichness"] = Json{{"voices", fl.paperRichness.voices},
                              {"mechanicsTarget", fl.paperRichness.mechanicsTarget},
                              {"enrichMechanics", fl.paperRichness.enrichMechanics},
                              {"enrichModel", fl.paperRichness.enrichModel}};
    f["userVoices"] = Json{{"enabled", fl.userVoices.enabled},
                           {"steamMaxReviews", fl.userVoices.steamMaxReviews},
                           {"steamLanguages", strings(fl.userVoices.steamLanguages)},
                           {"glabMaxImpressions", fl.userVoices.glabMaxImpressions},
                           {"voluptasBaseUrl", fl.userVoices.voluptasBaseUrl}};
    f["notionLinks"] = Json{{"maxDepth", fl.notionLinks.maxDepth}, {"maxPages", fl.notionLinks.maxPages}};
    f["improvement"] = Json{{"effectModel", fl.improvement.effectModel}};
    f["paperRefine"] = Json{{"enabled", fl.paperRefine.enabled}, {"model", fl.paperRefine.model}};
    f["foresight"] = Json{{"enabled", fl.foresight.enabled},
                          {"turns", fl.foresight.turns},
                          {"model", fl.foresight.model},
                          {"forecastModel", fl.foresight.forecastModel}};
    f["proxy"] = Json{{"enabled", fl.proxy.enabled},
                      {"budgetTokens", fl.proxy.budgetTokens},
                      {"maxBudgetTokens", fl.proxy.maxBudgetTokens},
                      {"stanceModel", fl.proxy.stanceModel},
                      {"requireEvalPassed", fl.proxy.requireEvalPassed}};
    f["anatomia"] = Json{{"enabled", fl.anatomia.enabled},
                         {"binPath", fl.anatomia.binPath},
                         {"autoDraft", fl.anatomia.autoDraft},
                         {"refineModel", fl.anatomia.refineModel},
                         {"timeoutMs", fl.anatomia.timeoutMs}};
    o["flow"] = std::move(f);
    o["workspace"] = c.workspace;

    Json dt = Json::object();
    put_opt(dt, "kuzuPath", c.discatier.kuzuPath);
    put_opt(dt, "activeKg", c.discatier.activeKg);
    if (c.discatier.knowledgeGraphs) {
        Json list = Json::array();
        for (const auto& kg : *c.discatier.knowledgeGraphs) {
            Json k = Json::object();
            k["id"] = kg.id;
            put_opt(k, "label", kg.label);
            k["kuzuPath"] = kg.kuzuPath;
            list.push_back(std::move(k));
        }
        dt["knowledgeGraphs"] = std::move(list);
    }
    o["discatier"] = std::move(dt);

    o["personaEngine"] = Json{{"enabled", c.personaEngine.enabled},
                              {"dbPath", c.personaEngine.dbPath},
                              {"maxFiresPerSession", c.personaEngine.maxFiresPerSession},
                              {"maxFiresPerRule", c.personaEngine.maxFiresPerRule},
                              {"tickMs", c.personaEngine.tickMs},
                              {"bridgePollMs", c.personaEngine.bridgePollMs}};
    o["facilitator"] = Json{{"enabled", c.facilitator.enabled},
                            {"tickMs", c.facilitator.tickMs},
                            {"idleGapMs", c.facilitator.idleGapMs},
                            {"maxPersonas", c.facilitator.maxPersonas},
                            {"aufhebungTarget", c.facilitator.aufhebungTarget}};
    o["autoSeed"] = Json{{"enabled", c.autoSeed.enabled},
                         {"intervalMs", c.autoSeed.intervalMs},
                         {"maxConcurrent", c.autoSeed.maxConcurrent},
                         {"sources", strings(c.autoSeed.sources)}};

    Json llm = Json::object();
    llm["backend"] = c.llm.backend;
    put_opt(llm, "anthropicApiKey", c.llm.anthropicApiKey);
    put_opt(llm, "model", c.llm.model);
    Json tiers = Json::object();
    put_opt(tiers, "cheap", c.llm.tiers.cheap);
    put_opt(tiers, "strong", c.llm.tiers.strong);
    llm["tiers"] = std::move(tiers);
    llm["claudeCliTimeoutMs"] = c.llm.claudeCliTimeoutMs;
    put_opt(llm, "gitBashPath", c.llm.gitBashPath);
    Json local = Json::object();
    local["baseUrl"] = c.llm.local.baseUrl;
    local["model"] = c.llm.local.model;
    put_opt(local, "apiKey", c.llm.local.apiKey);
    local["timeoutMs"] = c.llm.local.timeoutMs;
    llm["local"] = std::move(local);
    o["llm"] = std::move(llm);

    Json emb = Json::object();
    emb["enabled"] = c.embedding.enabled;
    emb["baseUrl"] = c.embedding.baseUrl;
    emb["model"] = c.embedding.model;
    put_opt(emb, "apiKey", c.embedding.apiKey);
    emb["timeoutMs"] = c.embedding.timeoutMs;
    emb["batchSize"] = c.embedding.batchSize;
    o["embedding"] = std::move(emb);

    o["classifier"] = Json{{"backend", c.classifier.backend}, {"model", c.classifier.model}, {"timeoutMs", c.classifier.timeoutMs}};

    Json opinions = Json::array();
    for (const auto& m : c.discussion.opinionModels) opinions.push_back(Json{{"model", m.model}, {"weight", m.weight}});
    o["discussion"] = Json{{"expectedUtterances", c.discussion.expectedUtterances},
                           {"keymanCount", c.discussion.keymanCount},
                           {"facilitatorModel", c.discussion.facilitatorModel},
                           {"keymanModel", c.discussion.keymanModel},
                           {"opinionModels", std::move(opinions)},
                           {"minTotal", c.discussion.minTotal},
                           {"turnDelayMs", c.discussion.turnDelayMs},
                           {"continueTimeoutMs", c.discussion.continueTimeoutMs},
                           {"maxRounds", c.discussion.maxRounds}};

    Json wp = Json::object();
    wp["enabled"] = c.workerPool.enabled;
    wp["workspace"] = c.workerPool.workspace;
    wp["callbackBaseUrl"] = c.workerPool.callbackBaseUrl;
    put_opt(wp, "gitBashPath", c.workerPool.gitBashPath);
    wp["injectDelayMs"] = c.workerPool.injectDelayMs;
    wp["turnTimeoutMs"] = c.workerPool.turnTimeoutMs;
    wp["registerTimeoutMs"] = c.workerPool.registerTimeoutMs;
    wp["registerSettleMs"] = c.workerPool.registerSettleMs;
    Json workers = Json::array();
    for (const auto& w : c.workerPool.workers)
        workers.push_back(Json{{"id", w.id}, {"role", w.role}, {"provider", w.provider}, {"model", w.model}});
    wp["workers"] = std::move(workers);
    wp["excludeProviders"] = strings(c.workerPool.excludeProviders);
    o["workerPool"] = std::move(wp);

    Json dc = Json::object();
    put_opt(dc, "botToken", c.discord.botToken);
    put_opt(dc, "applicationId", c.discord.applicationId);
    dc["guildIds"] = strings(c.discord.guildIds);
    dc["adminIds"] = strings(c.discord.adminIds);
    dc["discussionChannelIds"] = strings(c.discord.discussionChannelIds);
    dc["crawlChannelIds"] = strings(c.discord.crawlChannelIds);
    dc["forum"] = Json{{"enabled", c.discord.forum.enabled},
                       {"summaryChannelName", c.discord.forum.summaryChannelName},
                       {"dataLearningChannelName", c.discord.forum.dataLearningChannelName},
                       {"managedCategoryName", c.discord.forum.managedCategoryName},
                       {"improvementTagNames", strings(c.discord.forum.improvementTagNames)},
                       {"funTagNames", strings(c.discord.forum.funTagNames)},
                       {"discussionForumName", c.discord.forum.discussionForumName}};
    dc["gameFeedback"] = Json{{"enabled", c.discord.gameFeedback.enabled}, {"categoryName", c.discord.gameFeedback.categoryName}};
    o["discord"] = std::move(dc);

    o["slack"] = Json{{"enabled", c.slack.enabled},
                      {"appToken", c.slack.appToken},
                      {"botToken", c.slack.botToken},
                      {"channelIds", strings(c.slack.channelIds)},
                      {"adminIds", strings(c.slack.adminIds)}};

    Json bk = Json::object();
    bk["enabled"] = c.backup.enabled;
    put_opt(bk, "bucket", c.backup.bucket);
    bk["region"] = c.backup.region;
    bk["prefix"] = c.backup.prefix;
    bk["storageClass"] = c.backup.storageClass;
    put_opt(bk, "endpoint", c.backup.endpoint);
    put_opt(bk, "accessKeyId", c.backup.accessKeyId);
    put_opt(bk, "secretAccessKey", c.backup.secretAccessKey);
    bk["intervalDays"] = c.backup.intervalDays;
    bk["stateFile"] = c.backup.stateFile;
    o["backup"] = std::move(bk);

    o["fundamentum"] = Json{{"enabled", c.fundamentum.enabled}, {"dataDir", c.fundamentum.dataDir}};

    Json kg = Json::object();
    put_opt(kg, "sourceUrl", c.kgSync.sourceUrl);
    put_opt(kg, "token", c.kgSync.token);
    kg["auto"] = c.kgSync.auto_;
    kg["intervalHours"] = c.kgSync.intervalHours;
    kg["stateFile"] = c.kgSync.stateFile;
    put_opt(kg, "serveToken", c.kgSync.serveToken);
    o["kgSync"] = std::move(kg);

    Json relay = Json::object();
    relay["enabled"] = c.cost.relay.enabled;
    put_opt(relay, "anatomiaUrl", c.cost.relay.anatomiaUrl);
    put_opt(relay, "concordiaUrl", c.cost.relay.concordiaUrl);
    relay["intervalMs"] = c.cost.relay.intervalMs;
    relay["service"] = c.cost.relay.service;
    o["cost"] = Json{{"relay", std::move(relay)}};
    return o;
}

} // namespace di::config
