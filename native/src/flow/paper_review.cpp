#include "flow/paper_review.hpp"

#include <set>
#include <unordered_set>

#include "base/strings.hpp"
#include "flow/mechanic_extract.hpp"
#include "flow/paper_blocks.hpp"
#include "flow/project_premise.hpp"

namespace di::flow {

namespace {

void warn_if(const WarnFn& warn, const std::string& msg) {
    if (warn) warn(msg);
}

/** content.length > n ? content.slice(0, n) + "…" : content (UTF-16 単位)。 */
std::string clip(const std::string& content, std::size_t n) {
    if (str::js_length(content) > n) return str::js_slice(content, 0, static_cast<std::int64_t>(n)) + "…";
    return content;
}

PaperSample to_sample(const ContextVoice& v) { return PaperSample{clip(v.content, 120), v.source}; }

/**
 * 文字列から最初の JSON オブジェクトを取り出す (TS の extractJsonObject: 最初の `{` 〜 最後の `}` を JSON.parse)。
 * 救済パースはしない (TS と同じ)。上限つき。
 */
std::optional<Json> extract_object(std::string_view raw) {
    const auto start = raw.find('{');
    const auto end = raw.rfind('}');
    if (start == std::string_view::npos || end == std::string_view::npos || end <= start) return std::nullopt;
    auto parsed = parse_json(raw.substr(start, end - start + 1), llm_json_limits());
    if (!parsed || !parsed->is_object()) return std::nullopt;
    return parsed;
}

const Json* field(const Json& obj, const char* key) {
    if (!obj.is_object()) return nullptr;
    auto it = obj.find(key);
    return it == obj.end() ? nullptr : &*it;
}

/** typeof obj[key] === "string" ? 値 : nullopt */
std::optional<std::string> str_field(const Json& obj, const char* key) {
    const Json* v = field(obj, key);
    if (v && v->is_string()) return v->get<std::string>();
    return std::nullopt;
}

/** 任意の JSON 値を MechanicSummary に正規化する (name 必須)。source は有効値のみ保持。 */
std::optional<MechanicSummary> normalize_mechanic(const Json& raw) {
    if (!raw.is_object()) return std::nullopt;
    const std::string name = str::trim(str_field(raw, "name").value_or(""));
    if (name.empty()) return std::nullopt;
    const std::string affect = str::trim(str_field(raw, "intended_affect").value_or(""));
    MechanicSummary m;
    m.name = name;
    m.description = str::trim(str_field(raw, "description").value_or(""));
    if (!affect.empty()) m.intended_affect = affect;
    const Json* src = field(raw, "source");
    if (src) m.source = as_mechanic_source(*src);
    return m;
}

std::vector<MechanicSummary> normalize_mechanics(const Json& arr) {
    std::vector<MechanicSummary> out;
    for (const auto& v : arr)
        if (auto m = normalize_mechanic(v)) out.push_back(std::move(*m));
    return out;
}

/** 任意の JSON 値を issues[] に正規化する (文字列のみ・trim・空除外)。配列でなければ nullopt。 */
std::optional<std::vector<std::string>> sanitize_issues(const Json& raw) {
    if (!raw.is_array()) return std::nullopt;
    std::vector<std::string> issues;
    for (const auto& v : raw) {
        if (!v.is_string()) continue;
        auto s = str::trim(v.get<std::string>());
        if (!s.empty()) issues.push_back(std::move(s));
    }
    return issues;
}

/** 編集 LLM が返したタグ配列を有効タグだけに絞る (出現順・重複除去)。 */
std::vector<FlowTag> sanitize_tags(const Json& raw) {
    std::vector<FlowTag> out;
    if (!raw.is_array()) return out;
    const auto& valid = valid_flow_tags();
    for (const auto& t : raw) {
        if (!t.is_string()) continue;
        const auto s = t.get<std::string>();
        bool ok = false;
        for (const auto& v : valid) ok = ok || v == s;
        if (!ok) continue;
        bool dup = false;
        for (const auto& o : out) dup = dup || o == s;
        if (!dup) out.push_back(s);
    }
    return out;
}

std::optional<PaperFixedFields> normalize_seed(const std::string& theme, const std::optional<PaperFixedFields>& seed) {
    if (!seed) return std::nullopt;
    PaperFixedFields f;
    f.gameTitle = str::trim(seed->gameTitle);
    const auto dt = str::trim(seed->discussionTheme);
    f.discussionTheme = dt.empty() ? theme : dt;
    f.discussionContent = str::trim(seed->discussionContent);
    f.mechanicsContext = str::trim(seed->mechanicsContext);
    f.themeSupplement = str::trim(seed->themeSupplement);
    return f;
}

PaperUnderstanding deterministic_understanding(const PaperFixedFields& fields, const std::vector<MechanicSummary>& mechanics,
                                               const std::vector<ContextVoice>& voices, const std::optional<std::string>& flow) {
    std::vector<std::string> missing;
    if (requires_project_title(flow) && str::trim(fields.gameTitle).empty())
        missing.push_back("ゲームタイトル、またはプロジェクトの主目的を補足してください。");
    if (str::trim(fields.discussionTheme).empty()) missing.push_back("何について議論したいか、テーマを1つに絞って補足してください。");
    if (str::trim(fields.discussionContent).empty()) missing.push_back("議論で判断したい論点、前提、迷っている案を補足してください。");
    if (flow.value_or("") != "discussion" && str::trim(fields.mechanicsContext).empty() && mechanics.empty())
        missing.push_back("基本ループ、主要システム、操作、報酬、制約などゲーム内容が分かる説明を補足してください。");
    const bool has_external = !voices.empty() || !mechanics.empty();
    const bool ok = missing.empty() || (missing.size() <= 1 && has_external);
    PaperUnderstanding u;
    u.ok = ok;
    u.rationale = ok ? "入力内容と収集済み情報から、議論に必要な基本的なゲーム内容を把握できます。"
                     : "議論の前提となるゲーム内容が不足しているため、補足があると論点が安定します。";
    if (!ok) u.missingQuestions = std::move(missing);
    return u;
}

Json mechanic_for_edit(const MechanicSummary& m) {
    Json o = Json::object();
    o["name"] = m.name;
    o["description"] = m.description;
    o["intended_affect"] = m.intended_affect.value_or("");
    o["source"] = m.source.value_or("curated");
    return o;
}

/** JS の t.trim().toLowerCase().replace(/[。.!！\s]+$/, "") */
std::string normalize_reply_word(std::string_view text) {
    auto cps = str::codepoints(str::to_lower(str::trim(text)));
    while (!cps.empty()) {
        const auto c = cps.back();
        if (c == 0x3002 || c == '.' || c == '!' || c == 0xFF01 || str::is_js_space(c)) cps.pop_back();
        else break;
    }
    return str::from_codepoints(cps);
}

std::vector<std::string> strings_json(const std::vector<std::string>& v) { return v; }

} // namespace

const std::vector<FlowTag>& valid_flow_tags() {
    static const std::vector<FlowTag> tags{"機密", "内部", "運用", "開発", "未来予測"};
    return tags;
}

Json to_json(const PaperDraft& d) {
    Json o = to_json(static_cast<const PaperContent&>(d));
    o["bodyMd"] = d.bodyMd;
    return o;
}

PaperDraft paper_draft_from_json(const Json& j) {
    PaperDraft d;
    static_cast<PaperContent&>(d) = paper_content_from_json(j);
    d.bodyMd = json_string(j, "bodyMd");
    return d;
}

PaperDraft with_derived_body(const PaperContent& content) {
    PaperDraft d;
    static_cast<PaperContent&>(d) = content;
    d.bodyMd = paper_draft_to_markdown(content);
    return d;
}

PaperDraft with_derived_structure(const std::string& body_md, const PaperContent& fallback) {
    PaperDraft d;
    static_cast<PaperContent&>(d) = markdown_to_paper_draft(body_md, fallback);
    d.bodyMd = body_md;
    return d;
}

Json to_json(const PaperUnderstanding& u) {
    return Json{{"ok", u.ok}, {"rationale", u.rationale}, {"missingQuestions", strings_json(u.missingQuestions)}};
}

Json to_json(const PaperFixSuggestion& s) {
    Json o{{"title", s.title}, {"reason", s.reason}, {"suggestedChange", s.suggestedChange}};
    if (s.appliedAt) o["appliedAt"] = *s.appliedAt;
    if (s.appliedText) o["appliedText"] = *s.appliedText;
    return o;
}

Json to_json(const PaperMechanicsKnowledge& k) {
    return Json{{"ok", k.ok},
                {"confidence", k.confidence},
                {"summary", k.summary},
                {"knownMechanics", k.knownMechanics},
                {"missingQuestions", k.missingQuestions}};
}

Json to_json(const PaperVoiceSimulation& v) {
    return Json{{"possible", v.possible},
                {"confidence", v.confidence},
                {"summary", v.summary},
                {"caveat", v.caveat},
                {"nextAction", v.nextAction}};
}

Json to_json(const PaperReviewInfo& info) {
    Json o = Json::object();
    o["voiceCount"] = info.voiceCount;
    o["countCapped"] = info.countCapped;
    Json samples = Json::array();
    for (const auto& s : info.samples) samples.push_back(Json{{"content", s.content}, {"source", s.source}});
    o["samples"] = std::move(samples);
    if (info.understanding) o["understanding"] = to_json(*info.understanding);
    if (info.debatability) o["debatability"] = to_json(*info.debatability);
    if (info.fixSuggestions) {
        Json a = Json::array();
        for (const auto& s : *info.fixSuggestions) a.push_back(to_json(s));
        o["fixSuggestions"] = std::move(a);
    }
    if (info.mechanicsKnowledge) o["mechanicsKnowledge"] = to_json(*info.mechanicsKnowledge);
    if (info.voiceSimulation) o["voiceSimulation"] = to_json(*info.voiceSimulation);
    return o;
}

std::vector<MechanicSummary> merge_mechanics(const std::vector<MechanicSummary>& primary,
                                             const std::vector<MechanicSummary>& secondary) {
    std::unordered_set<std::string> seen;
    std::vector<MechanicSummary> out;
    auto take = [&](const MechanicSummary& m) {
        const auto key = str::to_lower(str::trim(m.name));
        if (key.empty() || seen.count(key)) return;
        seen.insert(key);
        out.push_back(m);
    };
    for (const auto& m : primary) take(m);
    for (const auto& m : secondary) take(m);
    return out;
}

PaperUnderstanding assess_paper_understanding(const PaperFixedFields& fields, const std::vector<MechanicSummary>& mechanics,
                                              const std::vector<ContextVoice>& voices, persona_engine::LLMClient* llm,
                                              const AssessUnderstandingOpts& opts) {
    auto fallback = deterministic_understanding(fields, mechanics, voices, opts.flow);
    if (!llm) return fallback;

    const bool discussion = opts.flow.value_or("") == "discussion";
    const std::string system =
        project_premise(opts.flow) + "\nあなたはゲーム議論を始める前の確認担当です。入力されたディスカッションペーパーを読み、" +
        (discussion ? "目的やアイデアから企画を議論できるか判定します。未定のプロジェクト名・既存実装・ゲームメカニクスは不足扱いにしません。"
                    : "AIが対象プロジェクトの現状を理解して議論できるか判定します。") +
        "情報が足りない場合は、ユーザに補足してほしい質問を返します。" +
        R"(JSON のみで {"ok":boolean,"rationale":string,"missingQuestions":string[]} を返してください。)";
    Json payload = Json::object();
    payload["fields"] = to_json(fields);
    Json mech = Json::array();
    for (std::size_t i = 0; i < mechanics.size() && i < 20; ++i) mech.push_back(to_json(mechanics[i]));
    payload["extractedMechanics"] = std::move(mech);
    payload["externalVoiceCount"] = voices.size();
    Json samples = Json::array();
    for (std::size_t i = 0; i < voices.size() && i < 5; ++i) samples.push_back(str::js_slice(voices[i].content, 0, 160));
    payload["externalVoiceSamples"] = std::move(samples);
    const std::string prompt = json_stringify_pretty(payload, 2);

    try {
        persona_engine::LLMInvokeArgs args;
        args.system = system;
        args.prompt = prompt;
        args.model = opts.model;
        const auto r = llm->invoke(args);
        if (!r.ok) {
            warn_if(opts.warn, "paper-understanding LLM エラー: " + r.error);
            return fallback;
        }
        const auto parsed = extract_object(r.text);
        if (!parsed) return fallback;
        const Json& o = *parsed;
        std::vector<std::string> missing;
        if (const Json* mq = field(o, "missingQuestions"); mq && mq->is_array()) {
            for (const auto& q : *mq) {
                if (!q.is_string()) continue;
                auto t = str::trim(q.get<std::string>());
                if (!t.empty()) missing.push_back(std::move(t));
            }
        }
        PaperUnderstanding u;
        const Json* okv = field(o, "ok");
        u.ok = (okv && okv->is_boolean()) ? okv->get<bool>() : missing.empty();
        const auto rationale = str::trim(str_field(o, "rationale").value_or(""));
        u.rationale = rationale.empty() ? fallback.rationale : rationale;
        u.missingQuestions = std::move(missing);
        return u;
    } catch (const std::exception& e) {
        warn_if(opts.warn, std::string("paper-understanding LLM 例外: ") + e.what());
        return fallback;
    }
}

PaperDraftBuild build_paper_draft(const std::string& theme, const std::vector<FlowTag>& tags, const BuildPaperDraftDeps& deps) {
    const auto seed = normalize_seed(theme, deps.seed);
    const bool discussion = deps.flow.value_or("") == "discussion";
    // 議論 (企画) は Di 内の data/games だけを読む: ゲーム名 (無ければ議題) に一致するゲームのメカニクスを載せ、
    // YouTube 等の外部収集はしない。一致しなければ空 (企画段階の新作)。
    std::vector<MechanicSummary> investigated;
    if (discussion) {
        const std::string key = (seed && !seed->gameTitle.empty()) ? seed->gameTitle : theme;
        investigated = load_theme_mechanics(deps.gamesDir.value_or("./data/games"), key, deps.warn);
    } else {
        InvestigateArgs args;
        args.theme = theme;
        args.tags = tags;
        if (deps.gamesDir) args.gamesDir = *deps.gamesDir;
        args.youtubeSearch = deps.youtubeSearch;
        if (deps.youtubeMaxComments) args.youtubeMaxComments = *deps.youtubeMaxComments;
        args.warn = deps.warn;
        investigated = investigate_theme(args).mechanics;
    }

    // 機密/内部では蓄積済みの外部の声もペーパーに載せない。
    const auto list_voices = gate_external_voices(tags, deps.listExternalVoices);
    std::vector<ContextVoice> voices;
    if (list_voices && *list_voices) voices = (*list_voices)({theme}, kPaperReviewCountLimit);

    // 事前情報 (Anatomia 由来など) を先頭に置き、investigate 結果と名前で重複排除する。
    const auto base = merge_mechanics(deps.extraMechanics, investigated);

    // メカニクスを LLM で目標件数まで増補 (議論は仮説メカニクスを混ぜないため増補しない)。
    std::vector<MechanicSummary> mechanics = base;
    const std::optional<std::string> enrich_model =
        (deps.enrichModel && !deps.enrichModel->empty()) ? deps.enrichModel : std::nullopt;
    if (deps.llm && !discussion) {
        EnrichMechanicsArgs args;
        args.theme = theme;
        args.existing = base;
        for (const auto& v : voices) args.voices.push_back(EnrichVoice{v.content, v.source});
        args.target = deps.mechanicsTarget.value_or(30);
        args.model = enrich_model;
        args.warn = deps.warn;
        mechanics = enrich_mechanics(*deps.llm, args);
    }

    std::string supplement = paper_supplement(tags);
    if (seed) {
        std::vector<std::string> parts;
        for (const auto& s : {paper_supplement(tags), seed->themeSupplement})
            if (!str::trim(s).empty()) parts.push_back(s);
        supplement = str::join(parts, "\n");
    }
    PaperContent base_content;
    base_content.theme = (seed && !seed->discussionTheme.empty()) ? seed->discussionTheme : theme;
    base_content.tags = tags;
    base_content.supplement = supplement;
    base_content.mechanics = mechanics;
    if (seed) {
        base_content.gameTitle = seed->gameTitle;
        base_content.discussionTheme = seed->discussionTheme;
        base_content.discussionContent = seed->discussionContent;
        base_content.mechanicsContext = seed->mechanicsContext;
        base_content.themeSupplement = seed->themeSupplement;
    }

    // 議論適性ゲート (09): 論点分解を前倒しして `# 論点` 節を草案に載せる。LLM 失敗は degraded で議論を止めない。
    std::optional<DebatabilityResult> debatability;
    if (deps.debatability && deps.debatability->llm) {
        AssessDebatabilityArgs args;
        args.theme = theme;
        std::vector<std::string> md_parts;
        const auto premise = project_premise(deps.flow);
        if (!premise.empty()) md_parts.push_back(premise);
        const auto md = paper_draft_to_markdown(base_content);
        if (!md.empty()) md_parts.push_back(md);
        args.paperMd = str::join(md_parts, "\n\n");
        for (const auto& v : voices) args.voices.push_back(DebatabilityVoice{v.content, v.source});
        args.minArmableIssues = deps.debatability->minArmableIssues;
        args.warn = deps.warn;
        debatability = deps.debatability->assess ? deps.debatability->assess(*deps.debatability->llm, args)
                                                 : assess_debatability(*deps.debatability->llm, args);
        auto issues = annotated_issues(*debatability);
        if (!issues.empty()) base_content.issues = std::move(issues);
    }

    PaperDraftBuild out;
    out.draft = with_derived_body(base_content);
    if (seed) {
        AssessUnderstandingOpts uo;
        uo.flow = deps.flow;
        uo.model = enrich_model;
        uo.warn = deps.warn;
        out.info.understanding =
            assess_paper_understanding(*seed, mechanics, voices, deps.understandingLlm ? deps.understandingLlm : deps.llm, uo);
    }
    out.info.voiceCount = static_cast<int>(voices.size());
    out.info.countCapped = static_cast<int>(voices.size()) >= kPaperReviewCountLimit;
    for (std::size_t i = 0; i < voices.size() && i < static_cast<std::size_t>(kPaperReviewSampleLimit); ++i)
        out.info.samples.push_back(to_sample(voices[i]));
    out.info.debatability = std::move(debatability);
    return out;
}

Json to_json(const PaperEditResult& r) {
    return Json{{"draft", to_json(r.draft)}, {"changeSummary", r.changeSummary}, {"applied", r.applied}};
}

PaperEditResult apply_paper_edit(const PaperDraft& draft, const std::string& instruction, persona_engine::LLMClient& llm,
                                 const ApplyPaperEditOpts& opts) {
    const auto trimmed = str::trim(instruction);
    if (trimmed.empty()) return {draft, "指示が空です。", false};

    Json current = Json::object();
    current["gameTitle"] = draft.gameTitle.value_or("");
    current["discussionTheme"] = draft.discussionTheme.value_or(draft.theme);
    current["discussionContent"] = draft.discussionContent.value_or("");
    current["mechanicsContext"] = draft.mechanicsContext.value_or("");
    current["themeSupplement"] = draft.themeSupplement.value_or(draft.supplement);
    current["theme"] = draft.theme;
    current["tags"] = draft.tags;
    current["supplement"] = draft.supplement;
    current["issues"] = draft.issues.value_or(std::vector<std::string>{});
    Json mech = Json::array();
    for (const auto& m : draft.mechanics) mech.push_back(mechanic_for_edit(m));
    current["mechanics"] = std::move(mech);

    std::string tag_list;
    for (const auto& t : valid_flow_tags()) tag_list += (tag_list.empty() ? "" : "/") + t;
    const std::string system =
        std::string("あなたは議論ブリーフ (ディスカッションペーパー) の編集者です。") +
        "ユーザの調整指示に従い、ペーパーを編集して **JSON のみ** で返してください。" +
        "指示に関係ないフィールドは現行値のまま保持します。" + "tags に使えるのは " + tag_list + " のみ。" +
        "mechanics の source (curated/llm/crawl = 出所ラベル) は指示がない限り現行値を保持します。" +
        "issues は議論の論点 (賛否が割れる争点) の配列です。" + "出力スキーマ: " +
        R"({"gameTitle":string,"discussionTheme":string,"discussionContent":string,"mechanicsContext":string,"themeSupplement":string,)" +
        R"("theme":string,"tags":string[],"supplement":string,"issues":string[],)" +
        R"("mechanics":[{"name":string,"description":string,"intended_affect":string,"source":string}],)" +
        R"("changeSummary":string}。)" + "changeSummary は変更点を 1 文 (日本語) で。前置きやコードフェンスは付けない。";
    const std::string prompt = "# 現行ペーパー\n" + json_stringify_pretty(current, 2) + "\n\n# 調整指示\n" + trimmed +
                               "\n\n上記指示を反映した編集後ペーパーを JSON で返してください。";

    persona_engine::LLMResult result;
    try {
        persona_engine::LLMInvokeArgs args;
        args.system = system;
        args.prompt = prompt;
        args.model = opts.model;
        result = llm.invoke(args);
    } catch (const std::exception& e) {
        warn_if(opts.warn, std::string("paper-edit LLM 例外: ") + e.what());
        return {draft, "編集に失敗しました (LLM 例外)。言い換えて再度お試しください。", false};
    }
    if (!result.ok) {
        warn_if(opts.warn, "paper-edit LLM エラー: " + result.error);
        return {draft, "編集に失敗しました。言い換えて再度お試しください。", false};
    }
    const auto parsed = extract_object(result.text);
    if (!parsed) return {draft, "指示を反映できませんでした。言い換えてください。", false};
    const Json& obj = *parsed;

    auto trimmed_or = [&](const char* key, const std::optional<std::string>& fb) -> std::optional<std::string> {
        if (auto s = str_field(obj, key)) return str::trim(*s);
        return fb;
    };
    PaperContent next;
    next.gameTitle = trimmed_or("gameTitle", draft.gameTitle);
    {
        const auto s = str::trim(str_field(obj, "discussionTheme").value_or(""));
        next.discussionTheme = s.empty() ? draft.discussionTheme : std::optional<std::string>(s);
    }
    next.discussionContent = trimmed_or("discussionContent", draft.discussionContent);
    next.mechanicsContext = trimmed_or("mechanicsContext", draft.mechanicsContext);
    next.themeSupplement = trimmed_or("themeSupplement", draft.themeSupplement);
    if (next.discussionTheme && !next.discussionTheme->empty()) {
        next.theme = *next.discussionTheme;
    } else {
        const auto t = str::trim(str_field(obj, "theme").value_or(""));
        next.theme = t.empty() ? draft.theme : t;
    }
    if (next.themeSupplement) next.supplement = *next.themeSupplement;
    else next.supplement = str_field(obj, "supplement") ? str::trim(*str_field(obj, "supplement")) : draft.supplement;
    next.tags = field(obj, "tags") ? sanitize_tags(*field(obj, "tags")) : draft.tags;
    const Json* mech_raw = field(obj, "mechanics");
    next.mechanics = (mech_raw && mech_raw->is_array()) ? normalize_mechanics(*mech_raw) : draft.mechanics;
    next.issues = field(obj, "issues") ? sanitize_issues(*field(obj, "issues")) : draft.issues;
    const auto summary = str::trim(str_field(obj, "changeSummary").value_or(""));

    // 構造化編集後に本文 md を再生成する (md を正本に保つ)。
    return {with_derived_body(next), summary.empty() ? "ペーパーを更新しました。" : summary, true};
}

PaperDraft coerce_paper_draft(const Json& raw, const PaperDraft& fallback) {
    // JS の typeof === "object" (配列も object。配列はプロパティを持たない空 object と同じに扱う)。
    if (!raw.is_object() && !raw.is_array()) return fallback;
    static const Json kEmpty = Json::object();
    const Json& o = raw.is_object() ? raw : kEmpty;
    // tags は本文 md に含めないので、本文 md 経路でも別途上書きを受ける。
    const std::vector<FlowTag> tags = field(o, "tags") ? sanitize_tags(*field(o, "tags")) : fallback.tags;
    // 本文 md があれば最優先 (md を正本に保つ)。
    if (auto body = str_field(o, "bodyMd"); body && !str::trim(*body).empty()) {
        PaperContent fb = fallback;
        fb.tags = tags;
        return with_derived_structure(*body, fb);
    }
    PaperContent c;
    const auto theme = str::trim(str_field(o, "theme").value_or(""));
    c.theme = theme.empty() ? fallback.theme : theme;
    c.tags = tags;
    c.supplement = str_field(o, "supplement").value_or(fallback.supplement);
    const Json* mech_raw = field(o, "mechanics");
    c.mechanics = (mech_raw && mech_raw->is_array()) ? normalize_mechanics(*mech_raw) : fallback.mechanics;
    c.issues = field(o, "issues") ? sanitize_issues(*field(o, "issues")) : fallback.issues;
    return with_derived_body(c);
}

std::string render_paper_review(const PaperDraft& draft, const PaperReviewInfo& info) {
    std::vector<std::string> lines;
    lines.push_back("📝 **ディスカッションペーパー (議論ブリーフ) — 確認してください**");
    lines.push_back("");
    lines.push_back("**議題**: " + draft.theme);
    if (!draft.tags.empty()) lines.push_back("**観点タグ**: " + str::join(draft.tags, " / "));
    if (!draft.supplement.empty()) lines.push_back("**観点補足**: " + draft.supplement);

    // 論点 (09): 争点分解の前倒し。人間が返信/編集で調整できる。
    if (draft.issues && !draft.issues->empty()) {
        lines.push_back("");
        lines.push_back("**論点**:");
        for (std::size_t i = 0; i < draft.issues->size(); ++i) lines.push_back(std::to_string(i + 1) + ". " + (*draft.issues)[i]);
    }

    lines.push_back("");
    const auto grounded = grounded_mechanics(draft.mechanics);
    const auto hypothetical = hypothetical_mechanics(draft.mechanics);
    if (!grounded.empty()) {
        lines.push_back("**ゲームのメカニクス**:");
        for (const auto& m : grounded) {
            const std::string affect = (m.intended_affect && !m.intended_affect->empty()) ? " → 期待感情: " + *m.intended_affect : "";
            lines.push_back("- " + m.name + ": " + m.description + affect + " （出所: " + m.source.value_or("curated") + "）");
        }
    } else {
        lines.push_back("**ゲームのメカニクス**: (なし)");
    }
    // 仮説メカニクス (08): LLM 増補分はバッジ付きで分離表示。
    if (!hypothetical.empty()) {
        lines.push_back("");
        lines.push_back("🧪 **仮説メカニクス (LLM抽出・未検証)**:");
        for (const auto& m : hypothetical) {
            const std::string affect = (m.intended_affect && !m.intended_affect->empty()) ? " → 期待感情: " + *m.intended_affect : "";
            lines.push_back("- " + m.name + ": " + m.description + affect);
        }
    }

    lines.push_back("");
    const std::string count_label = std::to_string(info.voiceCount) + (info.countCapped ? "+" : "");
    lines.push_back("**集めた情報**: 外部の声 " + count_label + " 件");
    for (const auto& s : info.samples) lines.push_back("- 「" + s.content + "」(出所: " + s.source + ")");

    // 議論適性ゲート (09): 議論不適なら再提案を併記する (人間が最終決定・強行可)。
    if (info.debatability) {
        const auto& d = *info.debatability;
        lines.push_back("");
        if (!d.degraded) {
            if (d.debatable) {
                lines.push_back("✅ " + d.message);
            } else {
                lines.push_back("⚠️ " + d.message);
                if (d.recommendation) lines.push_back("論点を追記・修正するか、このまま議論を開始できます。");
            }
        } else {
            lines.push_back("⚠️ " + d.message);
        }
    }
    return str::join(lines, "\n");
}

bool is_approval_text(std::string_view text) {
    static const std::set<std::string> exact{
        "開始", "承認", "ok", "okay", "go", "approve", "approved", "start", "これで開始", "これでいい", "これでok",
        "それで開始", "オッケー", "おっけー", "👍", "✅"};
    const auto t = normalize_reply_word(text);
    return !t.empty() && exact.count(t) > 0;
}

bool is_revert_text(std::string_view text) {
    static const std::set<std::string> exact{"戻す", "戻して", "もどす", "取り消し", "取消", "undo", "revert", "↶", "↩"};
    const auto t = normalize_reply_word(text);
    return !t.empty() && exact.count(t) > 0;
}

Json to_json(const BlockReviewResult& r) {
    return Json{{"blockId", r.blockId}, {"original", r.original}, {"proposed", r.proposed}, {"rationale", r.rationale}, {"ok", r.ok}};
}

BlockReviewResult review_block(persona_engine::LLMClient& llm, const ReviewBlockArgs& args) {
    const auto original = get_block_text(args.bodyMd, args.blockId);
    if (!original) return {args.blockId, "", "", "対象ブロックが見つかりません。", false};
    auto instruction = str::trim(args.instruction.value_or(""));
    if (instruction.empty()) instruction = "議論しやすく明確に整える";
    const std::string system =
        std::string("あなたは議論ブリーフ (ディスカッションペーパー) の編集補佐です。") +
        "渡された markdown ブロック 1 つを、議題ブリーフとして読みやすく改稿します。" +
        "見出し記号 (#) や箇条書き記号 (-) のブロック種別は保ち、意味を勝手に増やさない。" + "**JSON のみ** で返す。スキーマ: " +
        R"({"proposed":string,"rationale":string}。)" + "proposed は改稿後のブロック本文 (markdown)、rationale は変更点を 1 文 (日本語)。" +
        "前置きやコードフェンスは付けない。";
    const std::string prompt = "# 調整方針\n" + instruction + "\n\n# 対象ブロック (現行)\n" + *original + "\n\n# 参考: ブリーフ全文\n" +
                               args.bodyMd + "\n\n上記ブロックの改稿案を JSON で返してください。";

    persona_engine::LLMResult result;
    try {
        persona_engine::LLMInvokeArgs ia;
        ia.system = system;
        ia.prompt = prompt;
        ia.model = args.model;
        result = llm.invoke(ia);
    } catch (const std::exception& e) {
        warn_if(args.warn, std::string("reviewBlock LLM 例外: ") + e.what());
        return {args.blockId, *original, *original, "改稿に失敗しました (LLM 例外)。", false};
    }
    if (!result.ok) {
        warn_if(args.warn, "reviewBlock LLM エラー: " + result.error);
        return {args.blockId, *original, *original, "改稿に失敗しました。", false};
    }
    const auto parsed = extract_object(result.text);
    const auto proposed = parsed ? str::trim(str_field(*parsed, "proposed").value_or("")) : std::string();
    if (proposed.empty()) return {args.blockId, *original, *original, "改稿案を取得できませんでした。", false};
    const auto rationale = parsed ? str::trim(str_field(*parsed, "rationale").value_or("")) : std::string();
    return {args.blockId, *original, proposed, rationale.empty() ? "ブロックを改稿しました。" : rationale, true};
}

Json to_json(const EvidenceResult& r) {
    Json voices = Json::array();
    for (const auto& v : r.voices) voices.push_back(Json{{"content", v.content}, {"source", v.source}});
    return Json{{"voices", std::move(voices)}, {"suggestion", r.suggestion}};
}

EvidenceResult gather_evidence(const GatherEvidenceArgs& args) {
    const int limit = args.limit.value_or(12);
    EvidenceResult out;
    if (!args.listExternalVoices) return out;
    const auto raw = args.listExternalVoices({args.topic}, limit);
    for (std::size_t i = 0; i < raw.size() && static_cast<int>(i) < limit; ++i)
        out.voices.push_back(PaperSample{clip(raw[i].content, 200), raw[i].source});
    if (out.voices.empty()) return out;

    // 箇条書きの素の根拠 (LLM 無し時はこれを提案)。
    std::vector<std::string> bullet_lines;
    for (const auto& v : out.voices) bullet_lines.push_back("- 「" + v.content + "」（出所: " + v.source + "）");
    const auto bullets = str::join(bullet_lines, "\n");
    const std::string plain = "## 集めた根拠 (外部の声)\n" + bullets;
    if (!args.llm) {
        out.suggestion = plain;
        return out;
    }
    const std::string system = std::string("あなたは議論ブリーフの編集補佐です。集めた外部の声を踏まえ、議題に関する観点の") +
                               "根拠段落を中立的にまとめます。markdown の段落 (箇条書き可) のみを返す (前置き不要)。";
    const std::string prompt = "# 観点\n" + args.topic + "\n\n# 集めた外部の声\n" + bullets + "\n\n根拠段落をまとめてください。";
    try {
        persona_engine::LLMInvokeArgs ia;
        ia.system = system;
        ia.prompt = prompt;
        ia.model = args.model;
        const auto r = args.llm->invoke(ia);
        if (r.ok && !str::trim(r.text).empty()) {
            out.suggestion = str::trim(r.text);
            return out;
        }
    } catch (const std::exception& e) {
        warn_if(args.warn, std::string("gatherEvidence LLM 例外: ") + e.what());
    }
    out.suggestion = plain;
    return out;
}

} // namespace di::flow
