#include "flow/web/routes_internal.hpp"

namespace di::flow::web {

namespace routes_detail {


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


http::Response not_ready(const char* message) { return json_error(message, 409); }


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

} // namespace routes_detail
} // namespace di::flow::web
