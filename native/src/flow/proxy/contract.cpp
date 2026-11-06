#include "flow/proxy/contract.hpp"

#include <cmath>
#include <set>

#include "base/strings.hpp"
#include "flow/sentiment_vector.hpp"
#include "flow/text_scan.hpp"

namespace di::flow::proxy {

namespace {

constexpr std::size_t MAX_CLAIM_CHARS = 200;
constexpr std::size_t MAX_STYLE_NOTES = 4;
constexpr std::size_t MAX_STYLE_NOTE_CHARS = 80;
constexpr std::size_t MAX_HELD_OUT = 64;

using Errors = std::vector<std::string>;

// 個人を特定しうる / U3 で渡さないと決めたフィールド名。どの階層に現れても拒否する。
const std::set<std::string>& forbidden_keys() {
    static const std::set<std::string> keys = {"url",         "sourceUrl", "source_url", "authorId", "author_id",
                                               "authorName",  "author_name", "displayName", "display_name",
                                               "steamId",     "steam_id",  "userId",     "user_id"};
    return keys;
}

// /https?:\/\//i
bool has_url(std::string_view s) {
    const std::string lower = str::to_lower_ascii(s);
    return lower.find("http://") != std::string::npos || lower.find("https://") != std::string::npos;
}

bool is_lower_alnum(char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); }

bool is_card_id(std::string_view s) {
    if (s.size() < 2 || s.size() > 4 || s[0] != 'S') return false;
    for (std::size_t i = 1; i < s.size(); ++i)
        if (s[i] < '0' || s[i] > '9') return false;
    return true;
}

const Json& field(const Json& o, const char* key) {
    // 無いキー (undefined) を null と区別するため discarded を返す
    static const Json discarded = Json(Json::value_t::discarded);
    if (!o.is_object()) return discarded;
    auto it = o.find(key);
    return it == o.end() ? discarded : *it;
}

bool is_undefined(const Json& v) { return v.is_discarded(); }
bool is_nullish(const Json& v) { return v.is_discarded() || v.is_null(); }

const Json& obj_or_empty(const Json& v) {
    static const Json empty = Json::object();
    return v.is_object() ? v : empty;
}

void find_forbidden(const Json& v, const std::string& at, Errors& out) {
    if (v.is_string()) {
        if (has_url(v.get_ref<const std::string&>())) out.push_back(at + ": URL を含む値は受け取らない");
        return;
    }
    if (v.is_array()) {
        for (std::size_t i = 0; i < v.size(); ++i) find_forbidden(v[i], at + "[" + std::to_string(i) + "]", out);
        return;
    }
    if (!v.is_object()) return;
    for (auto it = v.begin(); it != v.end(); ++it) {
        if (forbidden_keys().count(it.key())) out.push_back(at + "." + it.key() + ": 個人を特定しうるフィールドは受け取らない");
        find_forbidden(it.value(), at + "." + it.key(), out);
    }
}

std::string str_field(const Json& v, const std::string& at, Errors& errors, std::size_t max = 2000) {
    if (!v.is_string() || str::trim(v.get_ref<const std::string&>()).empty()) {
        errors.push_back(at + ": 空でない文字列が必要");
        return "";
    }
    std::string s = str::trim(v.get_ref<const std::string&>());
    if (str::js_length(s) > max) errors.push_back(at + ": " + std::to_string(max) + " 文字以内");
    return s;
}

bool finite_number(const Json& v) { return v.is_number() && std::isfinite(v.get<double>()); }

double num(const Json& v, const std::string& at, Errors& errors) {
    if (!finite_number(v)) {
        errors.push_back(at + ": 有限の数値が必要");
        return 0;
    }
    return v.get<double>();
}

double non_neg_int(const Json& v, const std::string& at, Errors& errors) {
    const double n = num(v, at, errors);
    if (n != std::floor(n) || n < 0) errors.push_back(at + ": 0 以上の整数");
    return n;
}

std::vector<std::string> str_list(const Json& v, const std::string& at, Errors& errors, std::size_t max_items,
                                  std::size_t max_chars) {
    if (is_undefined(v)) return {};
    if (!v.is_array()) {
        errors.push_back(at + ": 配列が必要");
        return {};
    }
    if (v.size() > max_items) errors.push_back(at + ": " + std::to_string(max_items) + " 件以内");
    std::vector<std::string> out;
    for (std::size_t i = 0; i < v.size() && i < max_items; ++i)
        out.push_back(str_field(v[i], at + "[" + std::to_string(i) + "]", errors, max_chars));
    return out;
}

std::vector<double> vector20(const Json& v, const std::string& at, Errors& errors) {
    bool ok = v.is_array() && v.size() == flow::DIM;
    if (ok)
        for (const auto& x : v) ok = ok && finite_number(x);
    if (!ok) {
        errors.push_back(at + ": " + std::to_string(flow::DIM) + " 次元の有限値ベクトルが必要");
        return {};
    }
    std::vector<double> out;
    for (const auto& x : v) out.push_back(x.get<double>());
    return out;
}

std::string polarity(const Json& v, const std::string& at, Errors& errors) {
    if (v.is_string() && (v.get_ref<const std::string&>() == "pos" || v.get_ref<const std::string&>() == "neg"))
        return v.get<std::string>();
    errors.push_back(at + ": pos | neg");
    return "pos";
}

std::vector<std::string> evidence_ids(const Json& v, const std::string& at, Errors& errors, const std::set<std::string>& known) {
    auto ids = str_list(v, at, errors, MAX_EVIDENCE, 8);
    for (const auto& id : ids)
        if (!known.count(id)) errors.push_back(at + ": " + id + " はこのパックの evidence に無い");
    return ids;
}

std::vector<PackEvidence> parse_evidence(const Json& v, Errors& errors) {
    if (!v.is_array() || v.empty()) {
        errors.push_back("evidence: 1 件以上の配列が必要");
        return {};
    }
    if (v.size() > MAX_EVIDENCE) errors.push_back("evidence: " + std::to_string(MAX_EVIDENCE) + " 件以内");
    std::set<std::string> seen;
    std::vector<PackEvidence> out;
    for (std::size_t i = 0; i < v.size() && i < MAX_EVIDENCE; ++i) {
        const std::string at = "evidence[" + std::to_string(i) + "]";
        const Json& o = obj_or_empty(v[i]);
        PackEvidence e;
        e.id = str_field(field(o, "id"), at + ".id", errors, 8);
        if (!e.id.empty() && !text_scan::is_evidence_id(e.id)) errors.push_back(at + ".id: E<数字>");
        if (seen.count(e.id)) errors.push_back(at + ".id: " + e.id + " が重複");
        seen.insert(e.id);
        const Json& posted = field(o, "postedAt");
        if (!is_nullish(posted)) e.postedAt = num(posted, at + ".postedAt", errors);
        e.revision = str_field(field(o, "revision"), at + ".revision", errors, 128);
        e.text = str_field(field(o, "text"), at + ".text", errors, MAX_EVIDENCE_CHARS);
        e.sourceKind = str_field(field(o, "sourceKind"), at + ".sourceKind", errors, 32);
        e.gameTitle = str_field(field(o, "gameTitle"), at + ".gameTitle", errors, 200);
        e.polarity = polarity(field(o, "polarity"), at + ".polarity", errors);
        out.push_back(std::move(e));
    }
    return out;
}

std::vector<StanceCard> parse_stance_cards(const Json& v, Errors& errors, const std::set<std::string>& known) {
    if (is_undefined(v)) return {};
    if (!v.is_array()) {
        errors.push_back("stanceCards: 配列が必要");
        return {};
    }
    if (v.size() > MAX_STANCE_CARDS) errors.push_back("stanceCards: " + std::to_string(MAX_STANCE_CARDS) + " 件以内");
    std::vector<StanceCard> out;
    for (std::size_t i = 0; i < v.size() && i < MAX_STANCE_CARDS; ++i) {
        const std::string at = "stanceCards[" + std::to_string(i) + "]";
        const Json& o = obj_or_empty(v[i]);
        StanceCard c;
        c.id = str_field(field(o, "id"), at + ".id", errors, 8);
        if (!c.id.empty() && !is_card_id(c.id)) errors.push_back(at + ".id: S<数字>");
        c.evidenceIds = evidence_ids(field(o, "evidenceIds"), at + ".evidenceIds", errors, known);
        if (c.evidenceIds.empty()) errors.push_back(at + ".evidenceIds: 根拠の無い立場カードは受け取らない");
        const Json& ex = field(o, "extractor");
        std::optional<std::string> extractor;
        if (ex.is_string() && (ex.get_ref<const std::string&>() == "llm" || ex.get_ref<const std::string&>() == "rule"))
            extractor = ex.get<std::string>();
        if (!extractor) errors.push_back(at + ".extractor: rule | llm");
        c.claim = str_field(field(o, "claim"), at + ".claim", errors, MAX_CLAIM_CHARS);
        c.polarity = polarity(field(o, "polarity"), at + ".polarity", errors);
        c.extractor = extractor.value_or("rule");
        out.push_back(std::move(c));
    }
    return out;
}

std::vector<ConditionalGap> parse_conditional_gaps(const Json& v, Errors& errors, const std::set<std::string>& known) {
    if (is_undefined(v)) return {};
    if (!v.is_array()) {
        errors.push_back("conditionalGaps: 配列が必要");
        return {};
    }
    std::vector<ConditionalGap> out;
    for (std::size_t i = 0; i < v.size() && i < MAX_STANCE_CARDS; ++i) {
        const std::string at = "conditionalGaps[" + std::to_string(i) + "]";
        const Json& o = obj_or_empty(v[i]);
        ConditionalGap g;
        g.evidenceIds = evidence_ids(field(o, "evidenceIds"), at + ".evidenceIds", errors, known);
        if (g.evidenceIds.empty()) errors.push_back(at + ".evidenceIds: 根拠が必要");
        g.current = str_field(field(o, "current"), at + ".current", errors, MAX_CLAIM_CHARS);
        g.desired = str_field(field(o, "desired"), at + ".desired", errors, MAX_CLAIM_CHARS);
        const Json& cond = field(o, "condition");
        if (!is_nullish(cond)) g.condition = str_field(cond, at + ".condition", errors, MAX_CLAIM_CHARS);
        out.push_back(std::move(g));
    }
    return out;
}

PackSufficiency parse_sufficiency(const Json& v, Errors& errors) {
    const Json& o = obj_or_empty(v);
    if (!v.is_object()) errors.push_back("sufficiency: オブジェクトが必要");
    const Json& c = field(o, "confidence");
    std::optional<std::string> confidence;
    if (c.is_string()) {
        const auto& s = c.get_ref<const std::string&>();
        if (s == "high" || s == "medium" || s == "low") confidence = s;
    }
    if (!confidence) errors.push_back("sufficiency.confidence: high | medium | low");
    PackSufficiency out;
    out.utterances = non_neg_int(field(o, "utterances"), "sufficiency.utterances", errors);
    out.positive = non_neg_int(field(o, "positive"), "sufficiency.positive", errors);
    out.negative = non_neg_int(field(o, "negative"), "sufficiency.negative", errors);
    out.games = non_neg_int(field(o, "games"), "sufficiency.games", errors);
    out.aspects = non_neg_int(field(o, "aspects"), "sufficiency.aspects", errors);
    out.confidence = confidence.value_or("low");
    out.reasons = str_list(field(o, "reasons"), "sufficiency.reasons", errors, 8, 200);
    return out;
}

PackDesignGap parse_design_gap(const Json& v, Errors& errors) {
    const Json& o = obj_or_empty(v);
    if (!v.is_object()) errors.push_back("designGap: オブジェクトが必要");
    const Json& method = field(o, "method");
    if (!(method.is_string() && method.get_ref<const std::string&>() == "pos-minus-neg"))
        errors.push_back("designGap.method: pos-minus-neg");
    PackDesignGap out;
    const Json& vec = field(o, "vector");
    if (!is_nullish(vec)) out.vector = vector20(vec, "designGap.vector", errors);
    const Json& reason = field(o, "reason");
    if (!is_nullish(reason)) out.reason = str_field(reason, "designGap.reason", errors, 200);
    // vector === null && !reason (str は失敗時 "" を返す = 偽)
    if (!out.vector && (!out.reason || out.reason->empty())) errors.push_back("designGap.reason: vector が null なら理由が必要");
    out.wants = str_list(field(o, "wants"), "designGap.wants", errors, 6, 40);
    out.complaints = str_list(field(o, "complaints"), "designGap.complaints", errors, 6, 40);
    return out;
}

std::vector<HeldOutLabel> parse_held_out(const Json& v, Errors& errors) {
    if (is_undefined(v)) return {};
    if (!v.is_array()) {
        errors.push_back("heldOut: 配列が必要");
        return {};
    }
    std::vector<HeldOutLabel> out;
    for (std::size_t i = 0; i < v.size() && i < MAX_HELD_OUT; ++i) {
        const Json& o = obj_or_empty(v[i]);
        const Json& rec = field(o, "recommended");
        const std::string at = "heldOut[" + std::to_string(i) + "]";
        if (!rec.is_boolean()) errors.push_back(at + ".recommended: boolean が必要");
        HeldOutLabel h;
        h.game = str_field(field(o, "game"), at + ".game", errors, 200);
        h.recommended = rec.is_boolean() && rec.get<bool>();
        out.push_back(std::move(h));
    }
    return out;
}

Json opt_num(const std::optional<double>& v) { return v ? Json(*v) : Json(nullptr); }
Json opt_str(const std::optional<std::string>& v) { return v ? Json(*v) : Json(nullptr); }

} // namespace

bool is_speaker_key(std::string_view s) {
    if (!str::starts_with(s, "ext:")) return false;
    std::size_t i = 4;
    if (i >= s.size() || !(s[i] >= 'a' && s[i] <= 'z')) return false;
    ++i;
    while (i < s.size() && (is_lower_alnum(s[i]) || s[i] == '_' || s[i] == '-')) ++i;
    if (i >= s.size() || s[i] != ':') return false;
    ++i;
    if (s.size() - i != 16) return false;
    for (; i < s.size(); ++i)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return false;
    return true;
}

bool is_genre_id(std::string_view s) {
    if (s.empty() || s.size() > 64 || !is_lower_alnum(s[0])) return false;
    for (std::size_t i = 1; i < s.size(); ++i)
        if (!(is_lower_alnum(s[i]) || s[i] == '_' || s[i] == '-')) return false;
    return true;
}

PackValidation parse_persona_pack(const Json& raw) {
    PackValidation result;
    if (!raw.is_object()) {
        result.errors = {"パックはオブジェクトでなければならない"};
        return result;
    }
    Errors errors;
    find_forbidden(raw, "$", errors);

    const Json& cv = field(raw, "contractVersion");
    if (!(cv.is_number() && cv.get<double>() == PROXY_CONTRACT_VERSION))
        errors.push_back("contractVersion: " + std::to_string(PROXY_CONTRACT_VERSION) + " のみ対応");
    const Json& kind = field(raw, "personaKind");
    if (!(kind.is_string() && kind.get_ref<const std::string&>() == "observedSpeaker"))
        errors.push_back("personaKind: observedSpeaker のみ対応");
    PersonaPack pack;
    pack.speakerKey = str_field(field(raw, "speakerKey"), "speakerKey", errors, 80);
    if (!pack.speakerKey.empty() && !is_speaker_key(pack.speakerKey))
        errors.push_back("speakerKey: ext:<source>:<hmac16> の仮名のみ受け取る");
    pack.genre = str_field(field(raw, "genre"), "genre", errors, 64);
    if (!pack.genre.empty() && !is_genre_id(pack.genre)) errors.push_back("genre: 辞書 id (英小文字・数字・-_)");

    pack.evidence = parse_evidence(field(raw, "evidence"), errors);
    std::set<std::string> known;
    for (const auto& e : pack.evidence) known.insert(e.id);

    const Json& formation = obj_or_empty(field(raw, "formation"));
    const Json& method = field(formation, "method");
    std::optional<std::string> m;
    if (method.is_string() && (method.get_ref<const std::string&>() == "llm" || method.get_ref<const std::string&>() == "rule"))
        m = method.get<std::string>();
    if (!m) errors.push_back("formation.method: rule | llm");
    const Json& window = obj_or_empty(field(raw, "window"));
    pack.generatedAt = num(field(window, "generatedAt"), "window.generatedAt", errors);
    pack.expiresAt = num(field(window, "expiresAt"), "window.expiresAt", errors);
    if (pack.expiresAt <= pack.generatedAt) errors.push_back("window.expiresAt: generatedAt より後");
    const Json& affect = obj_or_empty(field(raw, "affect"));
    const Json& vsv = field(affect, "vectorSpecVersion");
    if (!(vsv.is_number() && vsv.get<double>() == flow::VECTOR_SPEC_VERSION))
        errors.push_back("affect.vectorSpecVersion: " + std::to_string(flow::VECTOR_SPEC_VERSION) + " のみ対応");
    const Json& revoked = field(raw, "revoked");
    if (!revoked.is_boolean()) errors.push_back("revoked: boolean が必要");

    // 以下はオブジェクトリテラルの評価順 (エラーの順序を TS と揃える)
    pack.genreSpecVersion = non_neg_int(field(raw, "genreSpecVersion"), "genreSpecVersion", errors);
    pack.profileVersion = str_field(field(raw, "profileVersion"), "profileVersion", errors, 128);
    pack.formationMethod = m.value_or("rule");
    pack.formationRuleVersion = str_field(field(formation, "ruleVersion"), "formation.ruleVersion", errors, 64);
    const Json& months = field(window, "observedMonths");
    if (!is_nullish(months)) pack.observedMonths = non_neg_int(months, "window.observedMonths", errors);
    pack.sufficiency = parse_sufficiency(field(raw, "sufficiency"), errors);
    pack.affectVector = vector20(field(affect, "vector"), "affect.vector", errors);
    pack.designGap = parse_design_gap(field(raw, "designGap"), errors);
    pack.conditionalGaps = parse_conditional_gaps(field(raw, "conditionalGaps"), errors, known);
    pack.stanceCards = parse_stance_cards(field(raw, "stanceCards"), errors, known);
    pack.styleNotes = str_list(field(raw, "styleNotes"), "styleNotes", errors, MAX_STYLE_NOTES, MAX_STYLE_NOTE_CHARS);
    pack.likedGames = str_list(field(raw, "likedGames"), "likedGames", errors, MAX_LIKED_GAMES, 200);
    pack.heldOut = parse_held_out(field(raw, "heldOut"), errors);
    pack.revoked = revoked.is_boolean() && revoked.get<bool>();

    if (!errors.empty()) {
        result.errors = std::move(errors);
        return result;
    }
    result.ok = true;
    result.pack = std::move(pack);
    return result;
}

Json to_json(const PersonaPack& p) {
    Json o = Json::object();
    o["contractVersion"] = p.contractVersion;
    o["personaKind"] = p.personaKind;
    o["speakerKey"] = p.speakerKey;
    o["genre"] = p.genre;
    o["genreSpecVersion"] = p.genreSpecVersion;
    o["profileVersion"] = p.profileVersion;
    o["formation"] = Json{{"method", p.formationMethod}, {"ruleVersion", p.formationRuleVersion}};
    Json window = Json::object();
    window["observedMonths"] = opt_num(p.observedMonths);
    window["generatedAt"] = p.generatedAt;
    window["expiresAt"] = p.expiresAt;
    o["window"] = std::move(window);
    Json s = Json::object();
    s["utterances"] = p.sufficiency.utterances;
    s["positive"] = p.sufficiency.positive;
    s["negative"] = p.sufficiency.negative;
    s["games"] = p.sufficiency.games;
    s["aspects"] = p.sufficiency.aspects;
    s["confidence"] = p.sufficiency.confidence;
    s["reasons"] = p.sufficiency.reasons;
    o["sufficiency"] = std::move(s);
    Json affect = Json::object();
    affect["vector"] = p.affectVector;
    affect["vectorSpecVersion"] = flow::VECTOR_SPEC_VERSION;
    o["affect"] = std::move(affect);
    Json dg = Json::object();
    dg["vector"] = p.designGap.vector ? Json(*p.designGap.vector) : Json(nullptr);
    dg["method"] = p.designGap.method;
    dg["wants"] = p.designGap.wants;
    dg["complaints"] = p.designGap.complaints;
    dg["reason"] = opt_str(p.designGap.reason);
    o["designGap"] = std::move(dg);
    Json gaps = Json::array();
    for (const auto& g : p.conditionalGaps) {
        Json j = Json::object();
        j["current"] = g.current;
        j["desired"] = g.desired;
        j["condition"] = opt_str(g.condition);
        j["evidenceIds"] = g.evidenceIds;
        gaps.push_back(std::move(j));
    }
    o["conditionalGaps"] = std::move(gaps);
    Json cards = Json::array();
    for (const auto& c : p.stanceCards) {
        Json j = Json::object();
        j["id"] = c.id;
        j["claim"] = c.claim;
        j["polarity"] = c.polarity;
        j["evidenceIds"] = c.evidenceIds;
        j["extractor"] = c.extractor;
        cards.push_back(std::move(j));
    }
    o["stanceCards"] = std::move(cards);
    Json ev = Json::array();
    for (const auto& e : p.evidence) {
        Json j = Json::object();
        j["id"] = e.id;
        j["revision"] = e.revision;
        j["text"] = e.text;
        j["sourceKind"] = e.sourceKind;
        j["gameTitle"] = e.gameTitle;
        j["polarity"] = e.polarity;
        j["postedAt"] = opt_num(e.postedAt);
        ev.push_back(std::move(j));
    }
    o["evidence"] = std::move(ev);
    o["styleNotes"] = p.styleNotes;
    o["likedGames"] = p.likedGames;
    Json held = Json::array();
    for (const auto& h : p.heldOut) held.push_back(Json{{"game", h.game}, {"recommended", h.recommended}});
    o["heldOut"] = std::move(held);
    o["revoked"] = p.revoked;
    return o;
}

Json to_json(const PackValidation& v) {
    Json o = Json::object();
    o["ok"] = v.ok;
    if (v.ok && v.pack) o["pack"] = to_json(*v.pack);
    else o["errors"] = v.errors;
    return o;
}

} // namespace di::flow::proxy
