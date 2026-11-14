// src/flow/web/routes.ts の移植。経路・JSON の形・status は TS と同じ。
#include "flow/web/routes.hpp"
#include "flow/web/routes_internal.hpp"
#include "flow/transcript/snapshot.hpp"



namespace di::flow::web {

namespace routes_detail {


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


http::Response handle_status(http::Request& req) {
    const std::string session_id = req.param("session");
    double since = 0;
    if (const auto n = api::js_query::parse_int(req.query("since").value_or("0")); n && *n != 0) since = *n;
    auto snap = transcript::load_transcript_snapshot(session_id, since);

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
    const bool done = (finished || (session && session->is_ended()) || snap.hasConclusionRow) && !foresight::is_foresight_active(session_id);

    Json j = Json::object();
    j["ok"] = true;
    j["utterances"] = std::move(snap.utterances);
    j["marks"] = std::move(snap.marks);
    j["conclusion"] = std::move(snap.conclusion);
    j["concluded"] = snap.concluded;
    // ライブのディスカッションペーパー本文 (議論進行で更新されていく)。
    j["paperMd"] = snap.paperMd ? Json(*snap.paperMd) : Json(nullptr);
    // 未来予測アドバイザリーの状態 (タグなし / 再起動後は null)。
    j["foresight"] = foresight ? to_json(*foresight) : Json(nullptr);
    j["done"] = done;
    return http::Response::json(j);
}

} // namespace routes_detail

using namespace routes_detail;

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
