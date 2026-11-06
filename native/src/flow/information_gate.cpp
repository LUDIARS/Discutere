#include "flow/information_gate.hpp"

#include <algorithm>

#include "base/strings.hpp"
#include "persona_engine/facilitator/prompts.hpp"

namespace di::flow {

bool is_density(const std::string& s) { return s == "sparse" || s == "moderate" || s == "rich"; }
bool is_density(const Json& v) { return v.is_string() && is_density(v.get<std::string>()); }

int density_rank(const InformationDensity& d) {
    if (d == "rich") return 2;
    if (d == "moderate") return 1;
    return 0;
}

Json to_json(const InformationGap& g) {
    Json j{{"aspect", g.aspect}, {"reason", g.reason}, {"query", g.query}};
    if (g.source) j["source"] = *g.source;
    return j;
}

Json to_json(const InformationEvaluation& e) {
    Json covered = Json::array();
    for (const auto& c : e.covered) covered.push_back(c);
    Json gaps = Json::array();
    for (const auto& g : e.gaps) gaps.push_back(to_json(g));
    Json j{{"density", e.density}, {"covered", covered}, {"gaps", gaps}};
    if (e.proposedRule) j["proposedRule"] = *e.proposedRule;
    return j;
}

InformationDensity fallback_density(std::size_t voice_count) {
    if (voice_count >= 8) return "rich";
    if (voice_count >= 3) return "moderate";
    return "sparse";
}

namespace {

InformationEvaluation fallback_evaluation(const std::vector<ContextVoice>& voices) {
    InformationEvaluation e;
    e.density = fallback_density(voices.size());
    return e;
}

void call(const std::function<void(const std::string&)>& fn, const std::string& m) {
    if (fn) fn(m);
}

} // namespace

std::vector<InformationGap> normalize_gaps(const Json& raw, const std::string& theme) {
    std::vector<InformationGap> out;
    if (!raw.is_array()) return out;
    for (const auto& g : raw) {
        if (out.size() >= kMaxGapItems) break;
        if (!g.is_object()) continue;
        const std::string aspect = g.contains("aspect") && g["aspect"].is_string() ? str::trim(g["aspect"].get<std::string>()) : "";
        if (aspect.empty()) continue;
        const std::string reason = g.contains("reason") && g["reason"].is_string() ? str::trim(g["reason"].get<std::string>()) : "";
        const std::string query_raw = g.contains("query") && g["query"].is_string() ? str::trim(g["query"].get<std::string>()) : "";
        const std::string query = !query_raw.empty() ? query_raw : str::trim(theme + " " + aspect);
        std::optional<AutoCrawlSource> source;
        if (g.contains("source") && g["source"].is_string() && is_auto_crawl_source(g["source"].get<std::string>()))
            source = g["source"].get<std::string>();
        out.push_back(InformationGap{aspect, reason, query, source});
    }
    return out;
}

std::string build_material_text(const std::string& theme, const std::vector<FlowTag>& tags, const std::vector<ContextVoice>& voices) {
    const std::string tag_text = tags.empty() ? std::string("(なし)") : str::join(tags, " / ");
    if (voices.empty()) return "議題: " + theme + "\n観点タグ: " + tag_text + "\n\n収集済みの外部の声: (0 件 — 材料なし)";
    std::vector<std::string> lines;
    const std::size_t n = std::min<std::size_t>(voices.size(), 30);
    for (std::size_t i = 0; i < n; ++i) {
        const auto& v = voices[i];
        lines.push_back(std::to_string(i + 1) + ". [" + v.source + "] " + str::js_slice(v.content, 0, 160) +
                        (str::js_length(v.content) > 160 ? "…" : ""));
    }
    return "議題: " + theme + "\n観点タグ: " + tag_text + "\n\n収集済みの外部の声 (" + std::to_string(voices.size()) + " 件):\n" +
           str::join(lines, "\n");
}

EvalPrompt build_eval_prompt(const std::string& material) {
    EvalPrompt p;
    p.system = std::string("あなたは議論の準備担当です。 与えられた議題と、 収集済みの『外部の声』(プレイヤー/視聴者の生の感想) を見て、") +
               " この議題で深い議論をするのに情報が足りているかを評価します。 量だけでなく、 観点の多様性" +
               " (肯定/否定/具体例/異なる立場) も評価軸にしてください。 返答は JSON のみ。";
    p.prompt = material + "\n\n" + "次の JSON だけを返してください (前後に説明文を付けない):\n" + "{\n" +
               "  \"density\": \"sparse\" | \"moderate\" | \"rich\",\n" + "  \"covered\": [\"既にカバーできている観点\", ...],\n" +
               "  \"gaps\": [\n" +
               "    { \"aspect\": \"不足している観点\", \"reason\": \"なぜ必要か\", \"query\": \"それを集める検索クエリ(日本語可)\", \"source\": \"niconico|youtube|steam|website|reddit\" }\n" +
               "  ],\n" +
               "  \"proposedRule\": { \"description\": \"…\", \"when\": { \"op\": \"and\", \"clauses\": [ { \"op\": \"cmp\", \"feature\": \"voiceCount\", \"cmp\": \">=\", \"value\": 8 }, { \"op\": \"cmp\", \"feature\": \"sourceKinds\", \"cmp\": \">=\", \"value\": 2 } ] } }\n" +
               "}\n" + "声が少ない / 観点が偏る (例: 肯定だけ・具体例なし) なら density=sparse とし、" +
               " gaps に不足観点と検索クエリを 1〜3 個挙げてください。 十分多様で厚いなら rich とし gaps は空配列で構いません。\n" +
               "proposedRule は任意です: この密度判定が単純な閾値で再現できるなら、" +
               " feature 名 voiceCount (件数) / sourceKinds (ソース種類数) / avgLen (平均文字数) / tagCount を使った" +
               " Condition を書いてください。自信が無ければ省略してください。";
    return p;
}

InformationEvaluation evaluate_information_density(const EvaluateArgs& args) {
    const EvalPrompt ep = build_eval_prompt(build_material_text(args.theme, args.tags, args.voices));
    persona_engine::LLMResult res;
    if (!args.llm) {
        call(args.warn, "情報密度評価で例外: LLM が未設定です");
        return fallback_evaluation(args.voices);
    }
    try {
        persona_engine::LLMInvokeArgs in;
        in.system = ep.system;
        in.prompt = ep.prompt;
        in.model = args.model;
        res = args.llm->invoke(in);
    } catch (const std::exception& e) {
        call(args.warn, std::string("情報密度評価で例外: ") + e.what());
        return fallback_evaluation(args.voices);
    }
    if (!res.ok) {
        call(args.warn, "情報密度評価に失敗: " + res.error);
        return fallback_evaluation(args.voices);
    }
    try {
        const Json parsed = persona_engine::facilitator::extract_json_object(res.text);
        if (!parsed.is_object()) throw std::runtime_error("expected JSON object");
        InformationEvaluation e;
        e.density = parsed.contains("density") && is_density(parsed["density"]) ? parsed["density"].get<std::string>()
                                                                                 : fallback_density(args.voices.size());
        if (parsed.contains("covered") && parsed["covered"].is_array())
            for (const auto& c : parsed["covered"]) {
                if (e.covered.size() >= kMaxCoveredItems) break;
                if (c.is_string()) e.covered.push_back(c.get<std::string>());
            }
        if (parsed.contains("gaps")) e.gaps = normalize_gaps(parsed["gaps"], args.theme);
        if (parsed.contains("proposedRule")) e.proposedRule = parsed["proposedRule"];
        return e;
    } catch (const std::exception& ex) {
        call(args.warn, std::string("情報密度評価の JSON 解析に失敗: ") + ex.what());
        return fallback_evaluation(args.voices);
    }
}

Json to_json(const InformationGateResult& r) {
    Json q = Json::array();
    for (const auto& s : r.crawledQueries) q.push_back(s);
    return Json{{"ran", r.ran},
                {"sufficient", r.sufficient},
                {"initialDensity", r.initialDensity},
                {"finalDensity", r.finalDensity},
                {"crawls", r.crawls},
                {"importedTotal", r.importedTotal},
                {"crawledQueries", q},
                {"message", r.message}};
}

InformationGateResult run_information_gate(const InformationGateDeps& deps) {
    const int max_items = deps.maxItems.value_or(200);
    const CollectAndImportFn do_collect = deps.collectAndImportFn ? deps.collectAndImportFn : CollectAndImportFn(collect_and_import);
    const EvaluateFn do_evaluate = deps.evaluateFn ? deps.evaluateFn : EvaluateFn(evaluate_information_density);
    // クロール対象ソース: crawlSources 優先、無ければ単数 crawlSource にフォールバック。
    std::vector<AutoCrawlSource> crawl_sources = deps.crawlSources;
    if (crawl_sources.empty() && deps.crawlSource && !deps.crawlSource->empty()) crawl_sources.push_back(*deps.crawlSource);

    auto list_voices = [&]() -> std::vector<ContextVoice> {
        if (!deps.listExternalVoices) return {};
        try {
            return deps.listExternalVoices({deps.theme}, ASSESS_LIMIT);
        } catch (...) {
            return {};
        }
    };
    auto evaluate = [&](std::vector<ContextVoice> voices) {
        EvaluateArgs a;
        a.theme = deps.theme;
        a.tags = deps.tags;
        a.voices = std::move(voices);
        a.llm = deps.llm;
        a.model = deps.config.model;
        a.warn = deps.warn;
        return do_evaluate(a);
    };

    // minDensity が不正なら TS (DENSITY_RANK[x] = undefined) と同じく学習せず sufficient=false。
    const bool valid_target = is_density(deps.config.minDensity);
    const int target = density_rank(deps.config.minDensity);
    InformationGateResult r;
    r.ran = true;

    InformationEvaluation evaluation = evaluate(list_voices());
    r.initialDensity = evaluation.density;

    while (valid_target && density_rank(evaluation.density) < target && r.crawls < deps.config.maxLearnIterations) {
        const std::size_t take = static_cast<std::size_t>(std::max(1, deps.config.maxGapsPerIteration));
        std::vector<std::string> queries;
        for (std::size_t i = 0; i < evaluation.gaps.size() && i < take; ++i) queries.push_back(evaluation.gaps[i].query);
        if (queries.empty()) queries.push_back(deps.theme);
        call(deps.log, "情報密度 " + evaluation.density + " < " + deps.config.minDensity + " → 不足観点 " + std::to_string(queries.size()) +
                           " 件を学習 (反復 " + std::to_string(r.crawls + 1) + "/" + std::to_string(deps.config.maxLearnIterations) + ")");
        for (const auto& query : queries) {
            // 各不足観点を全ソース横断で集める (niconico + youtube 等)。
            for (const auto& source : crawl_sources) {
                CollectAndImportDeps c;
                c.core = deps.core;
                c.theme = deps.theme;
                c.slug = derive_slug(deps.theme);
                c.workspaceId = deps.workspaceId;
                c.spec = AutoCrawlSpec{source, query, std::nullopt, std::nullopt, std::nullopt};
                c.maxItems = max_items;
                c.youtubeApiKey = deps.youtubeApiKey;
                c.log = deps.log;
                c.warn = deps.warn;
                try {
                    const auto got = do_collect(c);
                    r.importedTotal += got.imported;
                    r.crawledQueries.push_back(source + ":" + query);
                } catch (const std::exception& e) {
                    call(deps.warn, "学習クロール失敗 (source=" + source + " query=\"" + query + "\"): " + e.what());
                }
            }
        }
        r.crawls++;
        evaluation = evaluate(list_voices());
    }

    r.finalDensity = evaluation.density;
    r.sufficient = valid_target && density_rank(evaluation.density) >= target;
    const std::string stats = "学習 " + std::to_string(r.crawls) + " 回・取込 " + std::to_string(r.importedTotal) + " 件";
    r.message = r.sufficient ? "情報充分 (密度 " + r.initialDensity + "→" + r.finalDensity + " ≥ " + deps.config.minDensity + " / " + stats + ")"
                             : "目標 (" + deps.config.minDensity + ") に未達 (密度 " + r.initialDensity + "→" + r.finalDensity + " / " + stats +
                                   ") — このまま議論を開始";
    call(deps.log, r.message);
    return r;
}

} // namespace di::flow
