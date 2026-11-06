#include "flow/web/routes_internal.hpp"

namespace di::flow::web {

namespace routes_detail {

// ペーパー本文のブロック単位の編集 (LLM レビュー / 適用 / 根拠クロール / 戻す)。
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

} // namespace routes_detail
} // namespace di::flow::web
