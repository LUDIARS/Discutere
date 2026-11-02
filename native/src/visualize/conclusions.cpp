#include "visualize/conclusions.hpp"

#include <algorithm>

#include "base/strings.hpp"
#include "crawler/sources/persona.hpp"
#include "discord_hook/reactions.hpp"

namespace di::visualize {

namespace {

constexpr const char* kConvergePrefix = "【収束】";
constexpr std::int64_t kConvergePrefixJsLength = 4;  // "【収束】".length (UTF-16)
/** 論述データの行の上限 (DB の値も外部入力として課金する)。 */
constexpr sql::QueryLimits kDetailLimits{200'000, 256u << 20};

Json opt_json(const std::optional<std::string>& v) { return v ? Json(*v) : Json(nullptr); }

std::optional<std::string> opt_str(const Json& j, const char* key) {
    if (!j.is_object() || !j.contains(key) || !j[key].is_string()) return std::nullopt;
    return j[key].get<std::string>();
}
std::string str_or(const Json& j, const char* key) { return opt_str(j, key).value_or(""); }
std::int64_t int_or(const Json& j, const char* key) {
    if (!j.is_object() || !j.contains(key) || !j[key].is_number()) return 0;
    return static_cast<std::int64_t>(j[key].get<double>());
}
std::optional<std::int64_t> opt_int(const Json& j, const char* key) {
    if (!j.is_object() || !j.contains(key) || !j[key].is_number()) return std::nullopt;
    return static_cast<std::int64_t>(j[key].get<double>());
}

struct SourceRef {
    std::string source;
    std::string sourceUrl;
};

/** attribution-store から発話の出所を引く (個人マスクとは別レイヤー、§6)。無ければ nullopt。 */
std::optional<SourceRef> source_ref(crawler::sources::AttributionStore* attribution, const std::string& utterance_id) {
    if (!attribution) return std::nullopt;
    const auto a = attribution->get(utterance_id);
    if (!a) return std::nullopt;
    return SourceRef{a->source, a->sourceUrl};
}

std::optional<std::string> converge_text(sql::Database& raw, const std::string& session_id) {
    const auto row = raw.get(
        "SELECT raw_content FROM utterances WHERE session_id = ? AND raw_content LIKE ? ORDER BY posted_at DESC LIMIT 1",
        {session_id, std::string(kConvergePrefix) + "%"});
    if (!row) return std::nullopt;
    return str::trim(str::js_slice(row->str("raw_content"), kConvergePrefixJsLength));
}

std::int64_t utterance_count(sql::Database& raw, const std::string& session_id) {
    const auto row = raw.get("SELECT COUNT(*) AS n FROM utterances WHERE session_id = ?", {session_id});
    return row ? row->i64("n") : 0;
}

std::int64_t aufhebung_count(sql::Database& raw, const std::string& gap_id) {
    const auto row = raw.get("SELECT COUNT(*) AS n FROM aufhebung_stock WHERE gap_id = ?", {gap_id});
    return row ? row->i64("n") : 0;
}

std::vector<std::string> list_aufhebung(sql::Database& raw, const std::string& gap_id) {
    std::vector<std::string> out;
    for (const auto& r : raw.all("SELECT summary FROM aufhebung_stock WHERE gap_id = ? ORDER BY created_at ASC", {gap_id}, kDetailLimits))
        out.push_back(r.str("summary"));
    return out;
}

std::vector<ConclusionLine> transcript(sql::Database& raw, const std::string& session_id,
                                       crawler::sources::AttributionStore* attribution) {
    std::vector<ConclusionLine> out;
    for (const auto& r : raw.all("SELECT id, speaker_id, raw_content FROM utterances WHERE session_id = ? ORDER BY posted_at ASC",
                                 {session_id}, kDetailLimits)) {
        const auto ref = source_ref(attribution, r.str("id"));
        ConclusionLine line;
        line.speaker = conclusion_speaker_label(r.opt_str("speaker_id"));
        line.content = r.str("raw_content");
        if (ref) {
            line.source = ref->source;
            line.sourceUrl = ref->sourceUrl;
        }
        out.push_back(std::move(line));
    }
    return out;
}

std::vector<ConclusionOpinion> top_opinions(sql::Database& raw, const std::string& session_id, std::size_t limit,
                                            crawler::sources::AttributionStore* attribution) {
    std::vector<ConclusionOpinion> out;
    for (const auto& u : raw.all("SELECT id, speaker_id, raw_content FROM utterances WHERE session_id = ? AND speaker_id LIKE 'persona:%'",
                                 {session_id}, kDetailLimits)) {
        const std::string id = u.str("id");
        const auto ref = source_ref(attribution, id);
        ConclusionOpinion o;
        o.speaker = conclusion_speaker_label(u.opt_str("speaker_id"));
        o.content = u.str("raw_content");
        o.score = discord_hook::get_opinion_score(raw, id);
        if (ref) {
            o.source = ref->source;
            o.sourceUrl = ref->sourceUrl;
        }
        if (o.score > 0) out.push_back(std::move(o));
    }
    std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.score > b.score; });
    if (out.size() > limit) out.resize(limit);
    return out;
}

void put_summary_fields(Json& o, const ConclusionSummary& s) {
    o["gapId"] = s.gapId;
    o["kind"] = s.kind;
    o["sessionId"] = s.sessionId;
    o["title"] = s.title;
    o["conclusion"] = opt_json(s.conclusion);
    o["utteranceCount"] = s.utteranceCount;
    o["aufhebungCount"] = s.aufhebungCount;
    o["updatedAt"] = s.updatedAt;
    if (s.discussionVolume) o["discussionVolume"] = *s.discussionVolume;
    if (s.materialCount) o["materialCount"] = *s.materialCount;
}

void read_summary_fields(ConclusionSummary& s, const Json& j) {
    s.gapId = str_or(j, "gapId");
    s.kind = str_or(j, "kind");
    s.sessionId = str_or(j, "sessionId");
    s.title = str_or(j, "title");
    s.conclusion = opt_str(j, "conclusion");
    s.utteranceCount = int_or(j, "utteranceCount");
    s.aufhebungCount = int_or(j, "aufhebungCount");
    s.updatedAt = int_or(j, "updatedAt");
    s.discussionVolume = opt_int(j, "discussionVolume");
    s.materialCount = opt_int(j, "materialCount");
}

} // namespace

std::string conclusion_speaker_label(const std::optional<std::string>& speaker_id) {
    if (!speaker_id || speaker_id->empty()) return "参加者";
    if (str::starts_with(*speaker_id, "persona:")) return speaker_id->substr(8);
    if (str::starts_with(*speaker_id, "ext:")) return crawler::sources::masked_persona_label(*speaker_id);
    return "人間";
}

Json to_json(const ConclusionSummary& s) {
    Json o = Json::object();
    put_summary_fields(o, s);
    return o;
}

Json to_json(const ConclusionPaper& p) {
    Json mechanics = Json::array();
    for (const auto& m : p.mechanics) {
        Json mo = Json::object();
        mo["name"] = m.name;
        mo["description"] = m.description;
        if (m.intendedAffect) mo["intendedAffect"] = *m.intendedAffect;
        mechanics.push_back(std::move(mo));
    }
    Json o = Json::object();
    o["theme"] = p.theme;
    o["tags"] = p.tags;
    o["supplement"] = p.supplement;
    o["mechanics"] = std::move(mechanics);
    return o;
}

Json to_json(const ConclusionDetail& d) {
    Json o = Json::object();
    put_summary_fields(o, d);
    o["paper"] = d.paper ? to_json(*d.paper) : Json(nullptr);
    o["aufhebungen"] = d.aufhebungen;
    Json tops = Json::array();
    for (const auto& t : d.topOpinions) {
        Json x = Json::object();
        x["speaker"] = t.speaker;
        x["content"] = t.content;
        x["score"] = t.score;
        x["source"] = opt_json(t.source);
        x["sourceUrl"] = opt_json(t.sourceUrl);
        tops.push_back(std::move(x));
    }
    o["topOpinions"] = std::move(tops);
    Json lines = Json::array();
    for (const auto& t : d.transcript) {
        Json x = Json::object();
        x["speaker"] = t.speaker;
        x["content"] = t.content;
        x["source"] = opt_json(t.source);
        x["sourceUrl"] = opt_json(t.sourceUrl);
        lines.push_back(std::move(x));
    }
    o["transcript"] = std::move(lines);
    return o;
}

ConclusionSummary conclusion_summary_from_json(const Json& j) {
    ConclusionSummary s;
    read_summary_fields(s, j);
    return s;
}

ConclusionDetail conclusion_detail_from_json(const Json& j) {
    ConclusionDetail d;
    read_summary_fields(d, j);
    if (j.is_object() && j.contains("paper") && j["paper"].is_object()) {
        const Json& p = j["paper"];
        ConclusionPaper paper;
        paper.theme = str_or(p, "theme");
        if (p.contains("tags") && p["tags"].is_array())
            for (const auto& t : p["tags"])
                if (t.is_string()) paper.tags.push_back(t.get<std::string>());
        paper.supplement = str_or(p, "supplement");
        if (p.contains("mechanics") && p["mechanics"].is_array())
            for (const auto& m : p["mechanics"]) paper.mechanics.push_back({str_or(m, "name"), str_or(m, "description"), opt_str(m, "intendedAffect")});
        d.paper = std::move(paper);
    }
    if (j.is_object() && j.contains("aufhebungen") && j["aufhebungen"].is_array())
        for (const auto& a : j["aufhebungen"])
            if (a.is_string()) d.aufhebungen.push_back(a.get<std::string>());
    if (j.is_object() && j.contains("topOpinions") && j["topOpinions"].is_array())
        for (const auto& t : j["topOpinions"])
            d.topOpinions.push_back({str_or(t, "speaker"), str_or(t, "content"), int_or(t, "score"), opt_str(t, "source"), opt_str(t, "sourceUrl")});
    if (j.is_object() && j.contains("transcript") && j["transcript"].is_array())
        for (const auto& t : j["transcript"])
            d.transcript.push_back({str_or(t, "speaker"), str_or(t, "content"), opt_str(t, "source"), opt_str(t, "sourceUrl")});
    return d;
}

std::vector<ConclusionSummary> list_conclusions(core::Core& core, const std::string& workspace_id, std::int64_t limit) {
    auto& raw = core.raw();
    const auto rows = raw.all(
        R"SQL(SELECT g.id AS gapId, g.title AS title, g.updated_at AS updatedAt, s.id AS sessionId
         FROM design_gaps g
         JOIN sessions s
           ON s.workspace_id = g.workspace_id
          AND s.title = 'discussion-of-gap:' || g.id
        WHERE g.workspace_id = ?
          AND g.status IN ('closed', 'converged')
        ORDER BY g.updated_at DESC
        LIMIT ?)SQL",
        {workspace_id, limit}, kDetailLimits);
    std::vector<ConclusionSummary> out;
    for (const auto& r : rows) {
        ConclusionSummary s;
        s.gapId = r.str("gapId");
        s.kind = "gap";
        s.sessionId = r.str("sessionId");
        s.title = r.str("title");
        s.conclusion = converge_text(raw, s.sessionId);
        s.utteranceCount = utterance_count(raw, s.sessionId);
        s.aufhebungCount = aufhebung_count(raw, s.gapId);
        s.updatedAt = r.i64("updatedAt");
        out.push_back(std::move(s));
    }
    return out;
}

std::optional<ConclusionDetail> get_conclusion_detail(core::Core& core, const std::string& workspace_id, const std::string& gap_id,
                                                      crawler::sources::AttributionStore* attribution) {
    auto& raw = core.raw();
    const auto gap = raw.get("SELECT id, title, updated_at FROM design_gaps WHERE workspace_id = ? AND id = ?", {workspace_id, gap_id});
    if (!gap) return std::nullopt;
    const auto session = raw.get("SELECT id FROM sessions WHERE workspace_id = ? AND title = ? ORDER BY started_at DESC LIMIT 1",
                                 {workspace_id, "discussion-of-gap:" + gap_id});
    const std::string session_id = session ? session->str("id") : "";

    ConclusionDetail d;
    d.gapId = gap_id;
    d.kind = "gap";
    d.sessionId = session_id;
    d.title = gap->str("title");
    if (!session_id.empty()) d.conclusion = converge_text(raw, session_id);
    d.utteranceCount = session_id.empty() ? 0 : utterance_count(raw, session_id);
    d.aufhebungCount = aufhebung_count(raw, gap_id);
    d.updatedAt = gap->i64("updated_at");
    // 旧 design_gap 議論にはディスカッションペーパーが無い (新フローのみ)。
    d.aufhebungen = list_aufhebung(raw, gap_id);
    if (!session_id.empty()) {
        d.topOpinions = top_opinions(raw, session_id, 5, attribution);
        d.transcript = transcript(raw, session_id, attribution);
    }
    return d;
}

} // namespace di::visualize
