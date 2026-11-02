#include "visualize/flow_conclusions.hpp"

#include <algorithm>
#include <set>

#include "base/json.hpp"
#include "base/strings.hpp"
#include "flow/persona_display.hpp"

namespace di::visualize {

namespace {

/** flow_conclusion.summary の収束プレフィックス (flow/conclusion と一致)。 */
constexpr std::string_view kConvergePrefix = "【収束】";
/** 一覧・発話の行の上限 (DB の値も外部入力として課金する)。 */
constexpr sql::QueryLimits kRowLimits{200'000, 256u << 20};
const JsonLimits kColumnJsonLimits{kFlowConclusionJsonMaxBytes, 200'000, 32};

std::optional<Json> parse_column(const std::optional<std::string>& raw) {
    if (!raw) return std::nullopt;
    return parse_json(*raw, kColumnJsonLimits);
}

std::vector<std::string> parse_json_array(const std::optional<std::string>& raw) {
    std::vector<std::string> out;
    const auto parsed = parse_column(raw);
    if (!parsed || !parsed->is_array()) return out;
    for (const auto& x : *parsed)
        if (x.is_string()) out.push_back(x.get<std::string>());
    return out;
}

/** discussion_paper 由来の生 mechanics JSON 1 要素を表示用に正規化する。 */
std::optional<ConclusionMechanic> normalize_mechanic(const Json& raw) {
    if (!raw.is_object()) return std::nullopt;
    const std::string name = raw.contains("name") && raw["name"].is_string() ? raw["name"].get<std::string>() : "";
    if (name.empty()) return std::nullopt;
    ConclusionMechanic m;
    m.name = name;
    m.description = raw.contains("description") && raw["description"].is_string() ? raw["description"].get<std::string>() : "";
    if (raw.contains("intended_affect") && raw["intended_affect"].is_string()) m.intendedAffect = raw["intended_affect"].get<std::string>();
    return m;
}

std::vector<ConclusionMechanic> parse_mechanics(const std::optional<std::string>& raw) {
    std::vector<ConclusionMechanic> out;
    if (!raw || raw->empty()) return out;
    const auto parsed = parse_column(raw);
    if (!parsed || !parsed->is_array()) return out;
    for (const auto& m : *parsed)
        if (auto n = normalize_mechanic(m)) out.push_back(std::move(*n));
    return out;
}

/** 【収束】プレフィックスを外し、空 / 「(結論なし)」は nullopt にする。 */
std::optional<std::string> clean_summary(const std::string& summary, std::int64_t concluded) {
    if (!concluded) return std::nullopt;
    const std::string body = str::starts_with(summary, kConvergePrefix) ? str::trim(std::string_view(summary).substr(kConvergePrefix.size()))
                                                                         : str::trim(summary);
    if (body.empty() || body == "(結論なし)") return std::nullopt;
    return body;
}

std::string speaker_label(const sql::Row& u) {
    flow::DisplayNameInput in;
    in.name = u.str("persona_name");
    in.stance = u.str("stance");
    in.role = u.str("role");
    in.possessionName = u.opt_str("possession_name");
    return flow::compose_display_name(in);
}

std::int64_t utterance_count(sql::Database& db, const std::string& session_id) {
    const auto row = db.get("SELECT COUNT(*) AS n FROM flow_utterance WHERE session_id = ?", {session_id});
    return row ? row->i64("n") : 0;
}

std::int64_t vote_count(sql::Database& db, const std::string& utterance_id) {
    const auto row = db.get("SELECT COUNT(*) AS n FROM vote WHERE chosen_utterance_id = ?", {utterance_id});
    return row ? row->i64("n") : 0;
}

ConclusionSummary summary_from_row(const sql::Row& r, sql::Database& db) {
    ConclusionSummary s;
    const std::string session_id = r.str("session_id");
    s.gapId = std::string(FLOW_CONCLUSION_PREFIX) + session_id;
    s.kind = "flow";
    s.sessionId = session_id;
    const auto theme = r.opt_str("theme");
    s.title = theme ? *theme : "(無題の議論)";
    s.conclusion = clean_summary(r.str("summary"), r.i64("concluded"));
    s.utteranceCount = utterance_count(db, session_id);
    s.aufhebungCount = static_cast<std::int64_t>(parse_json_array(r.opt_str("aufhebung_json")).size());
    s.updatedAt = r.i64("created_at");
    return s;
}

/** discussion_paper の列からディスカッションペーパーを組み立てる (JOIN できなければ nullopt)。 */
std::optional<ConclusionPaper> build_paper(const sql::Row& r) {
    const auto theme = r.opt_str("theme");
    const auto tags_json = r.opt_str("tags_json");
    const auto mechanics_json = r.opt_str("mechanics_json");
    const auto supplement = r.opt_str("supplement");
    auto truthy = [](const std::optional<std::string>& v) { return v && !v->empty(); };
    if (!theme && !truthy(tags_json) && !truthy(mechanics_json) && !truthy(supplement)) return std::nullopt;
    ConclusionPaper p;
    p.theme = theme.value_or("");
    p.tags = parse_json_array(truthy(tags_json) ? tags_json : std::optional<std::string>("[]"));
    p.supplement = str::trim(supplement.value_or(""));
    p.mechanics = parse_mechanics(mechanics_json);
    return p;
}

} // namespace

bool is_flow_conclusion_id(std::string_view gap_id) { return str::starts_with(gap_id, FLOW_CONCLUSION_PREFIX); }

std::string flow_session_id_from_gap_id(std::string_view gap_id) {
    return std::string(gap_id.substr(std::min(gap_id.size(), FLOW_CONCLUSION_PREFIX.size())));
}

std::vector<ConclusionSummary> list_flow_conclusions(sql::Database& db, std::int64_t limit) {
    const auto rows = db.all(
        R"SQL(SELECT c.session_id, c.paper_id, c.summary, c.aufhebung_json, c.top_utterance_ids_json,
              c.concluded, c.created_at, p.theme AS theme
         FROM flow_conclusion c
         LEFT JOIN discussion_paper p ON p.id = c.paper_id
        ORDER BY c.created_at DESC
        LIMIT ?)SQL",
        {limit}, kRowLimits);
    std::vector<ConclusionSummary> out;
    out.reserve(rows.size());
    for (const auto& r : rows) out.push_back(summary_from_row(r, db));
    return out;
}

std::optional<ConclusionDetail> get_flow_conclusion_detail(sql::Database& db, const std::string& session_id) {
    const auto row = db.get(
        R"SQL(SELECT c.session_id, c.paper_id, c.summary, c.aufhebung_json, c.top_utterance_ids_json,
              c.concluded, c.created_at, p.theme AS theme,
              p.tags_json AS tags_json, p.mechanics_json AS mechanics_json, p.supplement AS supplement
         FROM flow_conclusion c
         LEFT JOIN discussion_paper p ON p.id = c.paper_id
        WHERE c.session_id = ?)SQL",
        {session_id});
    if (!row) return std::nullopt;

    ConclusionDetail d;
    static_cast<ConclusionSummary&>(d) = summary_from_row(*row, db);
    const auto transcript_rows = db.all(
        R"SQL(SELECT id, persona_name, role, stance, possession_name, text, is_error
         FROM flow_utterance
        WHERE session_id = ?
        ORDER BY created_at ASC)SQL",
        {session_id}, kRowLimits);

    // 高評価意見 = 投票で勝った発話 (top_utterance_ids_json)。スコアは vote 表の得票数。
    const auto top_list = parse_json_array(row->opt_str("top_utterance_ids_json"));
    const std::set<std::string> top_ids(top_list.begin(), top_list.end());
    for (const auto& u : transcript_rows) {
        if (!top_ids.count(u.str("id"))) continue;
        d.topOpinions.push_back({speaker_label(u), u.str("text"), vote_count(db, u.str("id")), std::nullopt, std::nullopt});
    }
    std::stable_sort(d.topOpinions.begin(), d.topOpinions.end(), [](const auto& a, const auto& b) { return a.score > b.score; });

    d.paper = build_paper(*row);
    d.aufhebungen = parse_json_array(row->opt_str("aufhebung_json"));
    for (const auto& u : transcript_rows) {
        if (u.i64("is_error")) continue;
        d.transcript.push_back({speaker_label(u), u.str("text"), std::nullopt, std::nullopt});
    }
    return d;
}

} // namespace di::visualize
