#include "flow/web/routes_internal.hpp"

namespace di::flow::web {

namespace routes_detail {


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

} // namespace routes_detail
} // namespace di::flow::web
