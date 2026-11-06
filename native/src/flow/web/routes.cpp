// src/flow/web/routes.ts の移植。経路・JSON の形・status は TS と同じ。
#include "flow/web/routes.hpp"
#include "flow/web/routes_internal.hpp"



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
