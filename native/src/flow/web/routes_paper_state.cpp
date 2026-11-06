#include "flow/web/routes_internal.hpp"

namespace di::flow::web {

namespace routes_detail {


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


std::string short_string(const Json* raw, const std::string& fallback) {
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


/**
 * PaperDraft の JSON。TS の固定欄つきドラフトは markdownToPaperDraft の spread の順で issues が固定欄の後ろ
 * (bodyMd の直前) に来るので、固定欄があるときはその順に並べ直す (キーの集合は同じ)。
 */
Json paper_json(const PaperDraft& draft) {
    Json j = to_json(draft);
    if (!j.contains("gameTitle") || !j.contains("issues")) return j;
    Json out = Json::object();
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (it.key() == "issues") continue;
        if (it.key() == "bodyMd") out["issues"] = j["issues"];
        out[it.key()] = it.value();
    }
    if (!out.contains("issues")) out["issues"] = j["issues"];
    return out;
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
    j["paper"] = paper_json(draft);
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


Json ok_with(const Json& payload, std::initializer_list<std::pair<const char*, Json>> before) {
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

} // namespace routes_detail
} // namespace di::flow::web
