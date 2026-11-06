#include "flow/discussion_paper.hpp"

#include <algorithm>
#include <stdexcept>

#include "base/crypto.hpp"
#include "base/strings.hpp"
#include "base/time.hpp"
#include "flow/db/connection.hpp"

namespace di::flow {

namespace {

/** DB の JSON 列を読む上限。 */
JsonLimits stored_json_limits() { return JsonLimits{16u << 20, 1'000'000, 64}; }

Json mechanics_json(const std::vector<MechanicSummary>& mechanics) {
    Json arr = Json::array();
    for (const auto& m : mechanics) arr.push_back(to_json(m));
    return arr;
}

std::string tags_json(const std::vector<FlowTag>& tags) { return json_stringify(Json(tags)); }

std::string build_user_opinions_text(const std::vector<ContextVoice>& voices) {
    if (voices.empty()) return "";
    std::vector<std::string> lines;
    for (std::size_t i = 0; i < voices.size() && i < 20; ++i) {
        const auto& v = voices[i];
        lines.push_back(std::to_string(i + 1) + ". " + str::js_slice(v.content, 0, 200) +
                        (str::js_length(v.content) > 200 ? "…" : "") + "（出所: " + v.source + "）");
    }
    return "## ユーザの声 (参考)\n" + str::join(lines, "\n");
}

std::string build_synthetic_opinions_text(const std::vector<std::string>& opinions) {
    if (opinions.empty()) return "";
    return "## 想定意見データ (synthetic / 実ユーザデータなし)\n" + str::join(opinions, "\n");
}

std::string format_previous_rounds(const std::vector<RoundSummary>& rounds) {
    if (rounds.empty()) return "(前ラウンドなし)";
    std::vector<std::string> parts;
    for (const auto& r : rounds) {
        const std::string auf = r.aufhebung.empty() ? "" : "\n  止揚: " + str::join(r.aufhebung, " / ");
        parts.push_back("### ラウンド " + std::to_string(r.round) + " まとめ\n" + r.summary + auf);
    }
    return str::join(parts, "\n\n");
}

DiscussionOriginUi default_origin_ui(const std::string& flow) { return flow == "sparring" ? "chat" : "ai"; }

std::int64_t next_discussion_no(sql::Database& db) {
    auto row = db.get("SELECT COALESCE(MAX(discussion_no), 0) + 1 AS n FROM discussion_paper");
    return row ? row->i64("n", 1) : 1;
}

std::vector<FlowTag> parse_tags(const std::string& s) {
    std::vector<FlowTag> out;
    auto v = parse_json(s, stored_json_limits());
    if (v && v->is_array())
        for (const auto& t : *v)
            if (t.is_string()) out.push_back(t.get<std::string>());
    return out;
}

std::vector<MechanicSummary> parse_mechanics_json(const std::string& s) {
    std::vector<MechanicSummary> out;
    auto v = parse_json(s, stored_json_limits());
    if (v && v->is_array())
        for (const auto& m : *v) out.push_back(mechanic_summary_from_json(m));
    return out;
}

DraftPaperRow stored_paper_to_draft(const sql::Row& row) {
    DraftPaperRow d;
    d.paperId = row.str("id");
    d.flow = row.str("flow");
    d.theme = row.str("theme");
    d.tags = parse_tags(row.str("tags_json"));
    d.mechanics = parse_mechanics_json(row.str("mechanics_json"));
    d.supplement = row.str("supplement");
    d.bodyMd = row.str("body_md");
    return d;
}

std::optional<Json> parse_stored_json(const std::optional<std::string>& raw) {
    if (!raw || raw->empty()) return std::nullopt;
    auto v = parse_json(*raw, stored_json_limits());
    // JSON.parse("null") は null → 偽として扱う (呼び出し側の truthy 判定と同じ)。
    if (!v || v->is_null()) return std::nullopt;
    return v;
}

/** persistPaper / persistDraftPaper の共通部。started=true なら status を 'started' にする。 */
std::string persist(const NewPaper& paper, const std::string& flow, const std::optional<DiscussionOriginUi>& origin_ui,
                    bool started) {
    auto db = db::get_flow_db();
    const auto now = time::now_ms();
    const std::string tags = tags_json(paper.tags);
    const std::string mechanics = json_stringify(mechanics_json(paper.mechanics));
    TitleCacheMeta meta{flow, origin_ui ? *origin_ui : default_origin_ui(flow)};
    auto existing = db->get("SELECT id, discussion_no FROM discussion_paper WHERE session_id = ?", {paper.sessionId});
    if (existing) {
        const auto discussion_no = existing->opt_i64("discussion_no").value_or(next_discussion_no(*db));
        const std::string id = existing->str("id");
        if (started) {
            db->run(R"SQL(UPDATE discussion_paper
          SET flow = ?, theme = ?, tags_json = ?, mechanics_json = ?, supplement = ?, body_md = ?,
              status = 'started', discussion_no = COALESCE(discussion_no, ?), updated_at = ?
        WHERE id = ?)SQL",
                    {flow, paper.theme, tags, mechanics, paper.supplement, sql::Value(paper.bodyMd), discussion_no, now, id});
        } else {
            db->run(R"SQL(UPDATE discussion_paper
          SET flow = ?, theme = ?, tags_json = ?, mechanics_json = ?, supplement = ?, body_md = ?,
              discussion_no = COALESCE(discussion_no, ?), updated_at = ?
        WHERE id = ?)SQL",
                    {flow, paper.theme, tags, mechanics, paper.supplement, sql::Value(paper.bodyMd), discussion_no, now, id});
        }
        upsert_discussion_title_cache(paper.sessionId, paper.theme, "paper", now, meta);
        return id;
    }
    const std::string paper_id = crypto::uuid_v4();
    const auto discussion_no = next_discussion_no(*db);
    db->run(std::string(R"SQL(INSERT INTO discussion_paper (id, flow, session_id, theme, tags_json, mechanics_json, supplement, body_md, status, discussion_no, created_at, updated_at)
     VALUES (?, ?, ?, ?, ?, ?, ?, ?, )SQL") +
                (started ? "'started'" : "'draft'") + ", ?, ?, ?)",
            {paper_id, flow, paper.sessionId, paper.theme, tags, mechanics, paper.supplement, sql::Value(paper.bodyMd),
             discussion_no, now, now});
    upsert_discussion_title_cache(paper.sessionId, paper.theme, "paper", now, meta);
    return paper_id;
}

} // namespace

Json to_json(const ContextVoice& v) {
    Json o = Json::object();
    o["content"] = v.content;
    o["source"] = v.source;
    if (v.sourceUrl) o["sourceUrl"] = *v.sourceUrl;
    return o;
}

ContextVoice context_voice_from_json(const Json& j) {
    ContextVoice v;
    v.content = json_string(j, "content");
    v.source = json_string(j, "source");
    v.sourceUrl = json_opt_string(j, "sourceUrl");
    return v;
}

std::vector<std::string> synthesize_opinions(const std::vector<MechanicSummary>& mechanics) {
    std::vector<std::string> out;
    for (const auto& m : mechanics) {
        if (!m.intended_affect || m.intended_affect->empty()) continue;
        out.push_back("[想定 (synthetic)] 「" + m.name + "」について: " + *m.intended_affect +
                      " という体験を期待している意見が考えられます。");
    }
    return out;
}

PersonaPaper build_persona_paper(const BuildPersonaPaperArgs& args) {
    const auto& paper = args.paper;
    const bool is_confidential = std::find(paper.tags.begin(), paper.tags.end(), "機密") != paper.tags.end();
    PersonaPaper p;
    p.theme = paper.theme;
    p.supplement = paper_supplement(paper.tags);
    p.mechanicsText = mechanics_body_markdown(paper.mechanics);
    p.previousRoundsText = format_previous_rounds(paper.rounds);
    const bool wants_opinions = args.persona.role == "opinion" || args.persona.isLocal;
    if (wants_opinions) {
        if (is_confidential) {
            // 機密: 想定意見データ (synthetic)
            p.userOpinionsText = build_synthetic_opinions_text(
                args.syntheticOpinions.empty() ? synthesize_opinions(paper.mechanics) : args.syntheticOpinions);
        } else {
            p.userOpinionsText = build_user_opinions_text(args.userVoices);
        }
    }
    for (const auto& u : args.currentRoundUtterances) p.currentRoundUtterances.push_back(u.personaName + ": " + u.text);
    p.bodyMd = paper.bodyMd;
    return p;
}

std::string build_paper_system(const PersonaPaper& p) {
    // 正本 markdown があればそれをそのまま system に載せる (各 LLM が md を直接参照)。
    if (p.bodyMd && !str::trim(*p.bodyMd).empty()) return *p.bodyMd;
    std::vector<std::string> parts{"# 議題\n" + p.theme};
    if (!p.supplement.empty()) parts.push_back("# 観点補足\n" + p.supplement);
    parts.push_back("# ゲームのメカニクス\n" + p.mechanicsText);
    return str::join(parts, "\n\n");
}

std::string stance_line(const FlowStance& stance) {
    if (stance == "neutral") return "あなたはファシリテーター。対立を整理し議論を前に進める。";
    if (stance == "pro") return "この議論でのあなたの立場は【賛成寄り】。主張を筋の通った形で擁護・補強する。";
    if (stance == "con") return "この議論でのあなたの立場は【反対寄り】。健全な反論・反例・見落とされた弱点を投げる。";
    if (stance == "opinion") return "あなたは意見役。ユーザの声を踏まえつつ自分の角度で意見を述べる。";
    if (stance == "reform")
        return "この議論でのあなたの立場は【改革派 (賛同)】。破壊的な一手を支持し、実行で良く変わることを擁護する。";
    if (stance == "conservative")
        return "この議論でのあなたの立場は【保守派 (否定)】。現行路線を守る理由と、破壊的な一手が失敗する筋書きを示す。";
    return "";
}

std::string build_persona_user_prompt(const PersonaPaper& p, const FlowStance& stance, const FlowPersona& persona) {
    std::vector<std::string> volatile_parts;
    if (p.previousRoundsText != "(前ラウンドなし)") volatile_parts.push_back("# 前ラウンドの結果\n" + p.previousRoundsText);
    if (!p.currentRoundUtterances.empty())
        volatile_parts.push_back("# 当ラウンドの意見 (これまで)\n" + str::join(p.currentRoundUtterances, "\n"));
    if (p.userOpinionsText && !p.userOpinionsText->empty()) volatile_parts.push_back(*p.userOpinionsText);

    std::vector<std::string> lines{"あなたは議論ペルソナ「" + persona.name + "」。",
                                   "特徴: " + str::join(persona.traits, " / ") + " / 話し方: " + persona.speechStyle};
    // 価値軸/核主張 (respec 01): persona-setup が生成した場合のみ注入する。
    if (persona.valueAxis && !persona.valueAxis->empty()) lines.push_back("あなたが重視する価値: " + *persona.valueAxis);
    // 憑依 (B, item4): データ由来の人物像を演じている場合、その感性を prompt に注入する。
    if (persona.possession)
        lines.push_back("あなたは今、データ由来の人物像「" + persona.possession->label + "」を憑依している。" +
                        persona.possession->descriptor.value_or(""));
    lines.push_back(stance_line(stance));
    if (persona.role == "debater" && persona.coreClaims && !persona.coreClaims->empty())
        lines.push_back("あなたの核となる主張: " + str::join(*persona.coreClaims, " / "));
    lines.emplace_back("Discord のチャットで実在の人間が話すように、自然な口語で 1〜2 文だけ書く。");
    lines.emplace_back("ラベルや箇条書きは使わない。既出の繰り返しは避け、議論を一歩進める。");
    lines.emplace_back(
        "直近の発言と同じ結論・同じ理由の言い換えは禁止。重複しそうなら、未検討のユーザ層、メカニクス条件、反例、判断基準のいずれかを新しく出す。");
    lines.emplace_back("合意できない場合も同じ賛否を往復せず、未決条件、検証方法、または小さな修正案に分解する。");
    lines.emplace_back("発言テキストのみを返す (JSON や前置きは不要)。付け足す事が無ければ空行のみ返す。");
    if (!volatile_parts.empty()) {
        lines.emplace_back("");
        lines.emplace_back("---");
        lines.insert(lines.end(), volatile_parts.begin(), volatile_parts.end());
    }
    return str::join(lines, "\n");
}

std::string paper_to_prompt(const PersonaPaper& p, const FlowStance& stance, const FlowPersona& persona) {
    return str::join({build_persona_user_prompt(p, stance, persona), "", "---", build_paper_system(p)}, "\n");
}

std::string normalize_discussion_display_title(std::string_view raw, std::string_view fallback) {
    std::string s(raw);
    const std::string prefix = "【収束】";
    if (str::starts_with(s, prefix)) s = str::trim_start(std::string_view(s).substr(prefix.size()));
    std::size_t start = 0;
    while (true) {
        const auto nl = s.find('\n', start);
        std::string_view piece = std::string_view(s).substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        if (nl != std::string::npos && !piece.empty() && piece.back() == '\r') piece.remove_suffix(1);
        const std::string t = str::trim(piece);
        if (!t.empty()) return t;
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return std::string(fallback);
}

void upsert_discussion_title_cache(const std::string& session_id, std::string_view raw_title,
                                   const DiscussionTitleSource& source, std::optional<std::int64_t> updated_at,
                                   const TitleCacheMeta& meta) {
    const std::string discussion_type = meta.discussionType.value_or("discussion");
    const std::string origin_ui = meta.originUi.value_or(default_origin_ui(discussion_type));
    db::get_flow_db()->run(
        R"SQL(INSERT INTO discussion_title_cache (session_id, title, source, updated_at, discussion_type, origin_ui)
       VALUES (?, ?, ?, ?, ?, ?)
       ON CONFLICT(session_id) DO UPDATE SET
         title = excluded.title,
         source = excluded.source,
         updated_at = excluded.updated_at,
         discussion_type = excluded.discussion_type,
         origin_ui = excluded.origin_ui)SQL",
        {session_id, normalize_discussion_display_title(raw_title), source, updated_at.value_or(time::now_ms()),
         discussion_type, origin_ui});
}

std::string persist_paper(const NewPaper& paper, const std::string& flow, const std::optional<DiscussionOriginUi>& origin_ui) {
    return persist(paper, flow, origin_ui, true);
}

std::string persist_draft_paper(const NewPaper& paper, const std::string& flow,
                                const std::optional<DiscussionOriginUi>& origin_ui) {
    return persist(paper, flow, origin_ui, false);
}

std::optional<DraftPaperRow> get_draft_paper(const std::string& session_id) {
    auto row = db::get_flow_db()->get(
        R"SQL(SELECT id, flow, theme, tags_json, mechanics_json, supplement, body_md
         FROM discussion_paper WHERE session_id = ? AND status = 'draft')SQL",
        {session_id});
    if (!row) return std::nullopt;
    return stored_paper_to_draft(*row);
}

std::optional<PaperSnapshotRow> get_paper_snapshot(const std::string& session_id) {
    auto row = db::get_flow_db()->get(
        R"SQL(SELECT id, flow, theme, tags_json, mechanics_json, supplement, body_md, status
         FROM discussion_paper WHERE session_id = ? ORDER BY created_at DESC LIMIT 1)SQL",
        {session_id});
    if (!row) return std::nullopt;
    PaperSnapshotRow out;
    static_cast<DraftPaperRow&>(out) = stored_paper_to_draft(*row);
    out.status = row->str("status");
    return out;
}

bool has_flow_conclusion(const std::string& session_id) {
    return db::get_flow_db()->get("SELECT 1 AS ok FROM flow_conclusion WHERE session_id = ? LIMIT 1", {session_id}).has_value();
}

void update_paper_body(const std::string& paper_id, const std::string& body_md) {
    db::get_flow_db()->run("UPDATE discussion_paper SET body_md = ?, updated_at = ? WHERE id = ?",
                           {body_md, time::now_ms(), paper_id});
}

void update_paper_derived(const std::string& paper_id, const std::string& supplement,
                          const std::vector<MechanicSummary>& mechanics) {
    db::get_flow_db()->run("UPDATE discussion_paper SET supplement = ?, mechanics_json = ?, updated_at = ? WHERE id = ?",
                           {supplement, json_stringify(mechanics_json(mechanics)), time::now_ms(), paper_id});
}

void set_paper_debatability(const std::string& session_id, const std::optional<Json>& result) {
    const bool empty = !result || result->is_null();
    db::get_flow_db()->run("UPDATE discussion_paper SET debatability_json = ?, updated_at = ? WHERE session_id = ?",
                           {empty ? sql::Value() : sql::Value(json_stringify(*result)), time::now_ms(), session_id});
}

void set_paper_review_info(const std::string& session_id, const std::optional<Json>& info) {
    const bool empty = !info || info->is_null();
    Json debatability;  // null
    if (!empty && info->is_object() && info->contains("debatability")) debatability = (*info)["debatability"];
    db::get_flow_db()->run(
        R"SQL(UPDATE discussion_paper
          SET review_info_json = ?, debatability_json = ?, updated_at = ?
        WHERE session_id = ?)SQL",
        {empty ? sql::Value() : sql::Value(json_stringify(*info)),
         debatability.is_null() ? sql::Value() : sql::Value(json_stringify(debatability)), time::now_ms(), session_id});
}

std::optional<Json> get_paper_review_info(const std::string& session_id) {
    auto row = db::get_flow_db()->get(
        R"SQL(SELECT review_info_json, debatability_json
         FROM discussion_paper
        WHERE session_id = ?
        ORDER BY created_at DESC
        LIMIT 1)SQL",
        {session_id});
    if (!row) return std::nullopt;
    auto info = parse_stored_json(row->opt_str("review_info_json"));
    if (info && json_truthy(*info)) return info;
    auto debatability = parse_stored_json(row->opt_str("debatability_json"));
    if (!debatability || !json_truthy(*debatability)) return std::nullopt;
    Json o = Json::object();
    o["voiceCount"] = 0;
    o["countCapped"] = false;
    o["samples"] = Json::array();
    o["debatability"] = *debatability;
    return o;
}

std::optional<std::string> get_paper_body_by_session(const std::string& session_id) {
    auto row = db::get_flow_db()->get(
        "SELECT body_md FROM discussion_paper WHERE session_id = ? ORDER BY created_at DESC LIMIT 1", {session_id});
    if (!row) return std::nullopt;
    return row->opt_str("body_md");
}

std::optional<std::int64_t> get_discussion_number_by_session(const std::string& session_id) {
    auto row = db::get_flow_db()->get(
        "SELECT discussion_no FROM discussion_paper WHERE session_id = ? ORDER BY created_at DESC LIMIT 1", {session_id});
    if (!row) return std::nullopt;
    return row->opt_i64("discussion_no");
}

Json to_json(const FlowSessionRow& r) {
    Json o = Json::object();
    o["discussionNo"] = r.discussionNo ? Json(*r.discussionNo) : Json(nullptr);
    o["sessionId"] = r.sessionId;
    o["title"] = r.title;
    o["theme"] = r.theme;
    o["flow"] = r.flow;
    o["status"] = r.status;
    o["discussionType"] = r.discussionType;
    o["originUi"] = r.originUi;
    o["createdAt"] = r.createdAt;
    o["updatedAt"] = r.updatedAt;
    o["utterances"] = r.utterances;
    o["concluded"] = r.concluded ? Json(*r.concluded) : Json(nullptr);
    return o;
}

namespace {

/** state 絞り込みの WHERE 句 (correlated subquery で結論有無を見る)。 */
const char* state_where(const FlowSessionState& state) {
    if (state == "all") return "1=1";
    if (state == "draft") return "dp.status = 'draft'";
    if (state == "live")
        return "dp.status != 'draft' AND COALESCE((SELECT fc.concluded FROM flow_conclusion fc WHERE fc.session_id = dp.session_id), 0) != 1";
    if (state == "concluded")
        return "COALESCE((SELECT fc.concluded FROM flow_conclusion fc WHERE fc.session_id = dp.session_id), 0) = 1";
    throw std::invalid_argument("unknown flow session state: " + state);
}

const char* scope_where(const FlowSessionScope& scope) {
    if (scope == "all") return "1=1";
    if (scope == "ai")
        return "COALESCE(tc.origin_ui, CASE WHEN dp.flow = 'sparring' THEN 'chat' ELSE 'ai' END) = 'ai'";
    if (scope == "chat")
        return "(COALESCE(tc.origin_ui, CASE WHEN dp.flow = 'sparring' THEN 'chat' ELSE 'ai' END) = 'chat' OR dp.flow = 'sparring')";
    throw std::invalid_argument("unknown flow session scope: " + scope);
}

} // namespace

ListFlowSessionsResult list_flow_sessions(const ListFlowSessionsOptions& opts) {
    auto db = db::get_flow_db();
    const std::string where = std::string(state_where(opts.state)) + " AND " + scope_where(opts.scope);
    ListFlowSessionsResult out;
    auto total = db->get(R"SQL(SELECT COUNT(*) AS n
           FROM discussion_paper dp
           LEFT JOIN discussion_title_cache tc ON tc.session_id = dp.session_id
          WHERE )SQL" + where);
    out.total = total ? total->i64("n") : 0;
    const auto rows = db->all(R"SQL(SELECT dp.discussion_no AS discussionNo,
              dp.session_id AS sessionId,
              COALESCE(NULLIF(tc.title, ''), dp.theme) AS title,
              dp.theme AS theme, dp.flow AS flow, dp.status AS status,
              COALESCE(tc.discussion_type, dp.flow) AS discussionType,
              COALESCE(tc.origin_ui, CASE WHEN dp.flow = 'sparring' THEN 'chat' ELSE 'ai' END) AS originUi,
              dp.created_at AS createdAt, dp.updated_at AS updatedAt,
              (SELECT COUNT(*) FROM flow_utterance fu WHERE fu.session_id = dp.session_id) AS utterances,
              (SELECT fc.concluded FROM flow_conclusion fc WHERE fc.session_id = dp.session_id) AS concluded
         FROM discussion_paper dp
         LEFT JOIN discussion_title_cache tc ON tc.session_id = dp.session_id
        WHERE )SQL" + where + R"SQL(
        ORDER BY dp.created_at DESC
        LIMIT ? OFFSET ?)SQL",
                              {opts.limit, opts.offset});
    for (const auto& r : rows) {
        FlowSessionRow s;
        s.discussionNo = r.opt_i64("discussionNo");
        s.sessionId = r.str("sessionId");
        s.title = r.str("title");
        s.theme = r.str("theme");
        s.flow = r.str("flow");
        s.status = r.str("status");
        s.discussionType = r.str("discussionType");
        s.originUi = r.str("originUi");
        s.createdAt = r.i64("createdAt");
        s.updatedAt = r.i64("updatedAt");
        s.utterances = r.i64("utterances");
        s.concluded = r.opt_i64("concluded");
        out.rows.push_back(std::move(s));
    }
    return out;
}

bool delete_flow_session(const std::string& session_id) {
    auto db = db::get_flow_db();
    const auto paper_rows = db->all("SELECT id FROM discussion_paper WHERE session_id = ?", {session_id});
    if (paper_rows.empty()) return false;
    db->transaction([&] {
        // paper_id 参照 (session_id を持たない append-only ラウンド) を先に消す。
        for (const auto& r : paper_rows) db->run("DELETE FROM discussion_paper_round WHERE paper_id = ?", {r.str("id")});
        // session_id を持つ派生表をすべて削除 (表名は固定の許可リスト)。
        for (const char* table : {"flow_utterance", "flow_conclusion", "vote", "improvement_score", "llm_call_log",
                                  "discussion_paper_revision", "conclusion_cache", "discussion_title_cache"})
            db->run(std::string("DELETE FROM ") + table + " WHERE session_id = ?", {session_id});
        db->run("DELETE FROM discussion_paper WHERE session_id = ?", {session_id});
    });
    return true;
}

} // namespace di::flow
