#include "visualize/md_exporter.hpp"

#include <cmath>

#include "base/strings.hpp"
#include "base/time.hpp"
#include "lib/front_matter/front_matter.hpp"

namespace di::visualize {

namespace {

/** 逆引き一覧の行数・バイト数の上限 (DB の値も外部入力)。 */
constexpr sql::QueryLimits kExportRowLimits{200'000, 128u << 20};
/** refs_json の上限。超えたものは壊れた値と同じく [] にする。 */
constexpr JsonLimits kRefsJsonLimits{4u << 20, 200'000, 16};
/** ISO 文字列の書式が変わらない範囲 (1970-01-01 .. 9999-12-31)。 */
constexpr double kMaxIsoMs = 253402300799999.0;

/** new Date(ts).toISOString()。範囲外 (TS では RangeError か書式の違う年) は std::range_error。 */
std::string ts_to_iso(const sql::Value& v) {
    double ms = v.is_null() ? 0.0 : v.as_double(std::nan(""));
    if (!std::isfinite(ms)) throw std::range_error("Invalid time value");
    ms = std::trunc(ms);
    if (ms < 0 || ms > kMaxIsoMs) throw std::range_error("Invalid time value");
    return time::iso_utc(static_cast<std::int64_t>(ms));
}

/** JS の truthy (文字列は空でない、数は 0 でない)。 */
bool truthy(const sql::Value& v) {
    if (v.is_null()) return false;
    if (v.is_text()) return !v.as_string().empty();
    if (v.is_number()) return v.as_double() != 0 && !std::isnan(v.as_double());
    return true;
}

/** `row.x === 1` */
bool is_one(const sql::Value& v) { return v.is_number() && v.as_double() == 1; }

std::vector<std::string> parse_refs_json(const sql::Value& raw) {
    if (!truthy(raw)) return {};
    auto parsed = parse_json(raw.as_string(), kRefsJsonLimits);
    std::vector<std::string> out;
    if (!parsed || !parsed->is_array()) return out;
    for (const auto& x : *parsed)
        if (x.is_string()) out.push_back(x.get<std::string>());
    return out;
}

Json links_json(const std::vector<Wikilink>& links) {
    Json arr = Json::array();
    for (const auto& l : links) arr.push_back(format_wikilink(l));
    return arr;
}

std::string render(const std::vector<std::string>& lines, const Json& fm) {
    return lib::front_matter::stringify("\n" + str::join(lines, "\n") + "\n", fm);
}

std::optional<sql::Row> fetch_row(core::Core& core, const char* table, const std::string& ws, const std::string& id) {
    return core.raw().get(std::string("SELECT * FROM ") + table + " WHERE workspace_id = ? AND id = ?", {ws, id});
}

} // namespace

Json to_json(const ExportResult& r) {
    Json outgoing = Json::array();
    for (const auto& l : r.outgoing) outgoing.push_back(visualize::to_json(l));
    return Json{{"type", r.type}, {"id", r.id}, {"markdown", r.markdown}, {"outgoing", outgoing}};
}

// ─────────────────────────── Hypothesis ───────────────────────────

ExportResult export_hypothesis(core::Core& core, const std::string& ws, const std::string& id) {
    const auto row = fetch_row(core, "hypotheses", ws, id);
    if (!row) throw ExporterError("hypothesis not found: " + id, "hyp", id);

    std::vector<Wikilink> refs;
    for (auto& utt_id : parse_refs_json((*row)["refs_json"])) refs.push_back({"utt", std::move(utt_id)});
    std::optional<Wikilink> addresses;
    if (truthy((*row)["design_gap_id"])) addresses = Wikilink{"gap", row->str("design_gap_id")};

    // 派生 mechanic: mechanics.intends に hypothesis.statement が含まれるかで素朴に逆引き
    // (Phase 0 の暫定。Phase 1 で events log から正確に追跡する)
    const std::string statement = row->str("statement");
    const auto proposed = core.raw().all("SELECT id, name FROM mechanics WHERE workspace_id = ? AND intends LIKE ?",
                                         {ws, "%" + str::js_slice(statement, 0, 32) + "%"}, kExportRowLimits);
    std::vector<Wikilink> proposes;
    for (const auto& m : proposed) proposes.push_back({"mch", m.str("id")});

    std::vector<std::string> validated_by;
    if (is_one((*row)["validated_by_emotion"])) validated_by.push_back("emotion");
    const bool integrated = is_one((*row)["integrated"]);
    const bool stale = is_one((*row)["stale_flagged"]);
    const std::string status = row->is_null("status") ? std::string("proposed") : row->str("status");

    Json fm = Json::object();
    fm["id"] = (*row)["id"].to_json();
    fm["type"] = "hypothesis";
    fm["workspace_id"] = (*row)["workspace_id"].to_json();
    fm["status"] = status;
    fm["statement"] = statement;
    fm["integrated"] = integrated;
    fm["stale_flagged"] = stale;
    fm["created_at"] = ts_to_iso((*row)["created_at"]);
    fm["updated_at"] = ts_to_iso((*row)["updated_at"]);
    if (addresses) fm["addresses"] = format_wikilink(*addresses);
    if (!proposes.empty()) fm["proposes"] = links_json(proposes);
    if (!refs.empty()) fm["refs"] = links_json(refs);
    if (!validated_by.empty()) fm["validated_by"] = validated_by;

    std::vector<std::string> lines;
    lines.push_back("# 仮説: " + statement);
    lines.push_back("");
    lines.push_back("## ステータス");
    lines.push_back("**" + status + "**" + (validated_by.empty() ? std::string() : " (validated_by: " + str::join(validated_by, ", ") + ")") +
                    (integrated ? " / integrated" : "") + (stale ? " / stale" : ""));
    if (!refs.empty()) {
        lines.push_back("");
        lines.push_back("## 根拠 (utterances)");
        for (const auto& ref : refs) lines.push_back("- " + format_wikilink(ref));
    }
    if (addresses) {
        lines.push_back("");
        lines.push_back("## 取り組む gap");
        lines.push_back(format_wikilink(*addresses));
    }
    if (!proposes.empty()) {
        lines.push_back("");
        lines.push_back("## 派生 mechanic");
        for (const auto& m : proposes) lines.push_back("- " + format_wikilink(m));
    }

    ExportResult r{"hyp", id, render(lines, fm), refs};
    r.outgoing.insert(r.outgoing.end(), proposes.begin(), proposes.end());
    if (addresses) r.outgoing.push_back(*addresses);
    return r;
}

// ─────────────────────────── Design Gap ───────────────────────────

ExportResult export_gap(core::Core& core, const std::string& ws, const std::string& id) {
    const auto row = fetch_row(core, "design_gaps", ws, id);
    if (!row) throw ExporterError("design gap not found: " + id, "gap", id);

    // この gap を addresses している hypothesis を逆引き
    const auto hyps = core.raw().all("SELECT id FROM hypotheses WHERE workspace_id = ? AND design_gap_id = ? ORDER BY updated_at DESC", {ws, id},
                                     kExportRowLimits);
    std::vector<Wikilink> hypotheses;
    for (const auto& h : hyps) hypotheses.push_back({"hyp", h.str("id")});

    const std::string title = row->str("title");
    Json fm = Json::object();
    fm["id"] = (*row)["id"].to_json();
    fm["type"] = "gap";
    fm["workspace_id"] = (*row)["workspace_id"].to_json();
    fm["title"] = title;
    fm["status"] = row->is_null("status") ? Json("open") : (*row)["status"].to_json();
    fm["created_at"] = ts_to_iso((*row)["created_at"]);
    fm["updated_at"] = ts_to_iso((*row)["updated_at"]);
    const bool has_gap_in = truthy((*row)["gap_in"]);
    const bool has_expected = truthy((*row)["expected_affect"]);
    const bool has_observed = truthy((*row)["observed_affect"]);
    const bool has_game = truthy((*row)["game_id"]);
    if (has_gap_in) fm["gap_in"] = (*row)["gap_in"].to_json();
    if (has_expected) fm["expected_affect"] = (*row)["expected_affect"].to_json();
    if (has_observed) fm["observed_affect"] = (*row)["observed_affect"].to_json();
    if (has_game) fm["game"] = format_wikilink({"game", row->str("game_id")});
    if (!hypotheses.empty()) fm["hypotheses"] = links_json(hypotheses);

    std::vector<std::string> lines;
    lines.push_back("# Design Gap: " + title);
    if (truthy((*row)["description"])) {
        lines.push_back("");
        lines.push_back(row->str("description"));
    }
    if (has_expected || has_observed) {
        lines.push_back("");
        lines.push_back("## 期待 vs 観測");
        lines.push_back("- 期待: **" + (row->is_null("expected_affect") ? std::string("?") : row->str("expected_affect")) + "**" +
                        (has_gap_in ? " (in " + row->str("gap_in") + ")" : std::string()));
        lines.push_back("- 観測: **" + (row->is_null("observed_affect") ? std::string("?") : row->str("observed_affect")) + "**");
    }
    if (!hypotheses.empty()) {
        lines.push_back("");
        lines.push_back("## 提案された hypotheses");
        for (const auto& h : hypotheses) lines.push_back("- " + format_wikilink(h));
    }

    ExportResult r{"gap", id, render(lines, fm), hypotheses};
    if (has_game) r.outgoing.push_back({"game", row->str("game_id")});
    return r;
}

// ─────────────────────────── Mechanic ───────────────────────────

ExportResult export_mechanic(core::Core& core, const std::string& ws, const std::string& id) {
    const auto row = fetch_row(core, "mechanics", ws, id);
    if (!row) throw ExporterError("mechanic not found: " + id, "mch", id);

    const std::string name = row->str("name");
    Json fm = Json::object();
    fm["id"] = (*row)["id"].to_json();
    fm["type"] = "mechanic";
    fm["workspace_id"] = (*row)["workspace_id"].to_json();
    fm["name"] = name;
    fm["created_at"] = ts_to_iso((*row)["created_at"]);
    fm["updated_at"] = ts_to_iso((*row)["updated_at"]);
    const bool has_desc = truthy((*row)["description"]);
    const bool has_intends = truthy((*row)["intends"]);
    const bool has_affect = truthy((*row)["intended_affect"]);
    if (has_desc) fm["description"] = (*row)["description"].to_json();
    if (has_intends) fm["intends"] = (*row)["intends"].to_json();
    if (has_affect) fm["intended_affect"] = (*row)["intended_affect"].to_json();
    std::optional<Wikilink> game_link;
    if (truthy((*row)["game_id"])) game_link = Wikilink{"game", row->str("game_id")};
    if (game_link) fm["game"] = format_wikilink(*game_link);

    std::vector<std::string> lines;
    lines.push_back("# Mechanic: " + name);
    if (has_desc) {
        lines.push_back("");
        lines.push_back(row->str("description"));
    }
    if (has_intends) {
        lines.push_back("");
        lines.push_back("## 設計意図");
        lines.push_back(row->str("intends"));
    }
    if (has_affect) {
        lines.push_back("");
        lines.push_back("## 狙う affect");
        lines.push_back("**" + row->str("intended_affect") + "**");
    }
    if (game_link) {
        lines.push_back("");
        lines.push_back("## ゲーム");
        lines.push_back(format_wikilink(*game_link));
    }

    ExportResult r{"mch", id, render(lines, fm), {}};
    if (game_link) r.outgoing.push_back(*game_link);
    return r;
}

// ─────────────────────────── Aesthetic ───────────────────────────

ExportResult export_aesthetic(core::Core& core, const std::string& ws, const std::string& id) {
    const auto row = fetch_row(core, "aesthetics", ws, id);
    if (!row) throw ExporterError("aesthetic not found: " + id, "aes", id);

    const std::string name = row->str("name");
    Json fm = Json::object();
    fm["id"] = (*row)["id"].to_json();
    fm["type"] = "aesthetic";
    fm["workspace_id"] = (*row)["workspace_id"].to_json();
    fm["name"] = name;
    fm["created_at"] = ts_to_iso((*row)["created_at"]);
    fm["updated_at"] = ts_to_iso((*row)["updated_at"]);
    const bool has_desc = truthy((*row)["description"]);
    if (has_desc) fm["description"] = (*row)["description"].to_json();
    std::optional<Wikilink> game_link;
    if (truthy((*row)["game_id"])) game_link = Wikilink{"game", row->str("game_id")};
    if (game_link) fm["game"] = format_wikilink(*game_link);

    std::vector<std::string> lines;
    lines.push_back("# Aesthetic: " + name);
    if (has_desc) {
        lines.push_back("");
        lines.push_back(row->str("description"));
    }
    if (game_link) {
        lines.push_back("");
        lines.push_back("## ゲーム");
        lines.push_back(format_wikilink(*game_link));
    }
    ExportResult r{"aes", id, render(lines, fm), {}};
    if (game_link) r.outgoing.push_back(*game_link);
    return r;
}

// ─────────────────────────── Utterance ───────────────────────────

ExportResult export_utterance(core::Core& core, const std::string& ws, const std::string& id) {
    const auto row = fetch_row(core, "utterances", ws, id);
    if (!row) throw ExporterError("utterance not found: " + id, "utt", id);

    const Wikilink session_link{"ses", row->str("session_id")};
    std::optional<Wikilink> responds_to_link;
    if (truthy((*row)["responds_to"])) responds_to_link = Wikilink{"utt", row->str("responds_to")};
    // この utterance を refs に持つ hypothesis を逆引き (refs_json に id 含む)
    const auto referenced_by = core.raw().all("SELECT id FROM hypotheses WHERE workspace_id = ? AND refs_json LIKE ? ORDER BY updated_at DESC",
                                              {ws, "%" + id + "%"}, kExportRowLimits);
    std::vector<Wikilink> ref_links;
    for (const auto& h : referenced_by) ref_links.push_back({"hyp", h.str("id")});

    Json fm = Json::object();
    fm["id"] = (*row)["id"].to_json();
    fm["type"] = "utterance";
    fm["workspace_id"] = (*row)["workspace_id"].to_json();
    fm["session"] = format_wikilink(session_link);
    fm["speaker"] = (*row)["speaker_id"].to_json();  // NULL は null
    fm["posted_at"] = ts_to_iso((*row)["posted_at"]);
    fm["created_at"] = ts_to_iso((*row)["created_at"]);
    if (responds_to_link) fm["responds_to"] = format_wikilink(*responds_to_link);
    if (!ref_links.empty()) fm["referenced_by"] = links_json(ref_links);

    const std::string raw_content = row->str("raw_content");
    std::vector<std::string> lines;
    lines.push_back("# Utterance");
    lines.push_back("");
    for (const auto& line : str::split(raw_content, "\n")) lines.push_back("> " + line);
    if (truthy((*row)["normalized_content"]) && row->str("normalized_content") != raw_content) {
        lines.push_back("");
        lines.push_back("## 正規化");
        lines.push_back(row->str("normalized_content"));
    }
    if (!ref_links.empty()) {
        lines.push_back("");
        lines.push_back("## 参照している hypothesis");
        for (const auto& h : ref_links) lines.push_back("- " + format_wikilink(h));
    }

    ExportResult r{"utt", id, render(lines, fm), {session_link}};
    r.outgoing.insert(r.outgoing.end(), ref_links.begin(), ref_links.end());
    if (responds_to_link) r.outgoing.push_back(*responds_to_link);
    return r;
}

// ─────────────────────────── Session ───────────────────────────

ExportResult export_session(core::Core& core, const std::string& ws, const std::string& id) {
    const auto row = fetch_row(core, "sessions", ws, id);
    if (!row) throw ExporterError("session not found: " + id, "ses", id);

    const auto utts = core.raw().all("SELECT id FROM utterances WHERE workspace_id = ? AND session_id = ? ORDER BY posted_at", {ws, id},
                                     kExportRowLimits);
    std::vector<Wikilink> utt_links;
    for (const auto& u : utts) utt_links.push_back({"utt", u.str("id")});

    const std::string title = row->str("title");
    Json fm = Json::object();
    fm["id"] = (*row)["id"].to_json();
    fm["type"] = "session";
    fm["workspace_id"] = (*row)["workspace_id"].to_json();
    fm["title"] = title;
    fm["started_at"] = ts_to_iso((*row)["started_at"]);
    fm["created_at"] = ts_to_iso((*row)["created_at"]);
    fm["updated_at"] = ts_to_iso((*row)["updated_at"]);
    const bool has_mode = truthy((*row)["mode"]);
    const bool has_scene = truthy((*row)["scene"]);
    if (truthy((*row)["ended_at"])) fm["ended_at"] = ts_to_iso((*row)["ended_at"]);
    if (has_mode) fm["mode"] = (*row)["mode"].to_json();
    if (has_scene) fm["scene"] = (*row)["scene"].to_json();
    if (!utt_links.empty()) fm["utterances"] = links_json(utt_links);

    std::vector<std::string> lines;
    lines.push_back("# Session: " + title);
    if (has_mode || has_scene) {
        lines.push_back("");
        lines.push_back("- mode: " + (row->is_null("mode") ? std::string("?") : row->str("mode")));
        if (has_scene) lines.push_back("- scene: " + row->str("scene"));
    }
    if (!utt_links.empty()) {
        lines.push_back("");
        lines.push_back("## Utterances (" + std::to_string(utt_links.size()) + " 件)");
        for (const auto& u : utt_links) lines.push_back("- " + format_wikilink(u));
    }
    return ExportResult{"ses", id, render(lines, fm), utt_links};
}

// ─────────────────────────── Dispatcher ───────────────────────────

Exporter exporter_for(std::string_view type) {
    if (type == "hyp") return &export_hypothesis;
    if (type == "gap") return &export_gap;
    if (type == "mch") return &export_mechanic;
    if (type == "aes") return &export_aesthetic;
    if (type == "utt") return &export_utterance;
    if (type == "ses") return &export_session;
    // affect / game は Phase 0 では未対応 (game は crawler 側で md 既に生成、affect は使用頻度低)。
    // persona は persona-engine の独立 DB に保存されているので dump_persona を直接使う。
    return nullptr;
}

ExportResult export_node(core::Core& core, const std::string& type, const std::string& ws, const std::string& id) {
    const Exporter fn = exporter_for(type);
    if (!fn) throw ExporterError("exporter not implemented for type: " + type, type, id);
    return fn(core, ws, id);
}

} // namespace di::visualize
