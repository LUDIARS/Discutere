#include "persona_engine/engine/handler.hpp"

#include <cstdint>
#include <set>
#include <string_view>
#include <vector>

#include "base/strings.hpp"
#include "persona_engine/seeds/rules.hpp"

namespace di::persona_engine::engine {

namespace {

// ─── 禁則パターン (TS の正規表現 /i を線形の走査で) ─────────────────────
// 入力は ASCII だけ小文字化して照合する (JS の /i は非 Unicode モードで、非 ASCII 文字が
// ASCII に畳まれることは無い。パターンの英字はすべて ASCII なので ASCII の大小無視と一致する)。

struct Elem {
    enum class Kind { lit, opt_char, opt_lit, space_star, alt } kind;
    std::string text;               // lit / opt_lit / opt_char (候補の文字の集合)
    std::vector<std::string> alts;  // alt
};

Elem lit(std::string s) { return {Elem::Kind::lit, std::move(s), {}}; }
Elem sep() { return {Elem::Kind::opt_char, "-_ ", {}}; }  // [-_ ]?
Elem opt(std::string s) { return {Elem::Kind::opt_lit, std::move(s), {}}; }
Elem spaces() { return {Elem::Kind::space_star, {}, {}}; }  // \s*
Elem alt(std::vector<std::string> a) { return {Elem::Kind::alt, {}, std::move(a)}; }

bool match_at(std::string_view s, std::size_t pos, const std::vector<Elem>& seq, std::size_t ei) {
    if (ei == seq.size()) return true;
    const Elem& e = seq[ei];
    switch (e.kind) {
    case Elem::Kind::lit:
        return s.substr(pos, e.text.size()) == e.text && match_at(s, pos + e.text.size(), seq, ei + 1);
    case Elem::Kind::opt_lit:
        if (s.substr(pos, e.text.size()) == e.text && match_at(s, pos + e.text.size(), seq, ei + 1)) return true;
        return match_at(s, pos, seq, ei + 1);
    case Elem::Kind::opt_char:
        if (pos < s.size() && e.text.find(s[pos]) != std::string::npos && match_at(s, pos + 1, seq, ei + 1)) return true;
        return match_at(s, pos, seq, ei + 1);
    case Elem::Kind::alt:
        for (const auto& a : e.alts)
            if (s.substr(pos, a.size()) == a && match_at(s, pos + a.size(), seq, ei + 1)) return true;
        return false;
    case Elem::Kind::space_star: {
        // 貪欲に空白を集め、長い方から後続を試す (正規表現の後戻りと同じ結果)。
        std::vector<std::size_t> ends{pos};
        std::size_t i = pos;
        while (i < s.size()) {
            std::size_t j = i;
            if (!str::is_js_space(str::next_codepoint(s, j))) break;
            i = j;
            ends.push_back(i);
        }
        for (auto it = ends.rbegin(); it != ends.rend(); ++it)
            if (match_at(s, *it, seq, ei + 1)) return true;
        return false;
    }
    }
    return false;
}

bool search(std::string_view s, const std::vector<Elem>& seq) {
    for (std::size_t pos = 0; pos <= s.size(); ++pos)
        if (match_at(s, pos, seq, 0)) return true;
    return false;
}

struct ForbiddenPattern {
    std::vector<std::vector<Elem>> alternatives;
    std::string reason;
};

const std::vector<ForbiddenPattern>& forbidden_patterns() {
    static const std::vector<ForbiddenPattern> patterns = {
        // /無音|沈黙|silence|quiet[-_ ]?check/i
        {{{lit("無音")}, {lit("沈黙")}, {lit("silence")}, {lit("quiet"), sep(), lit("check")}},
         "無音 / 沈黙確認系 rule は禁止 (rule_log で見えるため不要)"},
        // /進捗(どう|確認|聞|を聞|ping)|progress[-_ ]?(check|ping)/i
        {{{lit("進捗"), alt({"どう", "確認", "聞", "を聞", "ping"})}, {lit("progress"), sep(), alt({"check", "ping"})}},
         "進捗確認系 rule は禁止 (status endpoint で取得可)"},
        // /汎用雑談|general[-_ ]?chitchat|small[-_ ]?talk[-_ ]?random/i
        {{{lit("汎用雑談")}, {lit("general"), sep(), lit("chitchat")}, {lit("small"), sep(), lit("talk"), sep(), lit("random")}},
         "ロール無関係な汎用雑談 rule は禁止 (persona の役割に紐づけること)"},
        // /全\s*persona|all[-_ ]?personas?[-_ ]?(fire|act|invoke)/i
        {{{lit("全"), spaces(), lit("persona")}, {lit("all"), sep(), lit("persona"), opt("s"), sep(), alt({"fire", "act", "invoke"})}},
         "全 persona 一括動員 rule は禁止 (LLM コスト暴騰防止)"},
    };
    return patterns;
}

/** /^[a-z][a-z0-9-]{1,63}$/ */
bool valid_rule_id(std::string_view id) {
    if (id.size() < 2 || id.size() > 64) return false;
    if (id[0] < 'a' || id[0] > 'z') return false;
    for (std::size_t i = 1; i < id.size(); ++i) {
        const char c = id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
    }
    return true;
}

bool is_object(const std::optional<Json>& v) { return v && v->is_object(); }

std::optional<std::string> str_field(const Json& j, const char* key) {
    auto it = j.find(key);
    if (it != j.end() && it->is_string()) return it->get<std::string>();
    return std::nullopt;
}

std::optional<double> num_field(const Json& j, const char* key) {
    auto it = j.find(key);
    if (it != j.end() && it->is_number()) return it->get<double>();
    return std::nullopt;
}

void log_rule(const HandleActionArgs& args, const std::optional<std::string>& rule_id, const std::string& action,
              const std::string& detail) {
    args.rules->log({rule_id, action, "ai", detail});
}

HandleActionResult log_err(const HandleActionArgs& args, const std::string& detail) {
    log_rule(args, args.ruleId, "error", detail);
    args.logger->warn(Json{{"rule_id", args.ruleId}}, detail);
    return {"error", detail, std::nullopt};
}

std::optional<std::string> infer_design_gap_id_from_trigger(const std::optional<TriggerEvent>& trigger) {
    if (!trigger) return std::nullopt;
    if (trigger->kind != "DesignGapDetected" && trigger->kind != "DesignGapUpdated") return std::nullopt;
    if (!trigger->payload || !trigger->payload->is_object()) return std::nullopt;
    auto id = str_field(*trigger->payload, "id");
    if (id && !id->empty()) return id;
    return std::nullopt;
}

HandleActionResult handle_add_rule(const HandleActionArgs& args, const Json& json) {
    auto it = json.find("rule");
    // TS: !ruleSpec || typeof ruleSpec !== "object" (配列も object として通る → 各フィールドが undefined)
    if (it == json.end() || !(it->is_object() || it->is_array())) return log_err(args, "add_rule without rule object");
    static const Json kEmpty = Json::object();
    const Json& r = it->is_object() ? *it : kEmpty;
    const std::string id = str::trim(str_field(r, "id").value_or(""));
    const std::string trigger = str_field(r, "trigger_type").value_or("");
    const std::string instructions = str_field(r, "instructions").value_or("");
    if (!valid_rule_id(id)) return log_err(args, "add_rule id invalid (must be kebab 2-64): " + id);
    if (trigger != "tick" && trigger != "event") return log_err(args, "add_rule trigger_type must be tick|event: " + trigger);
    const std::size_t len = str::js_length(instructions);
    if (instructions.empty() || len < 16 || len > 4000) return log_err(args, "add_rule instructions length must be 16-4000");

    const auto description = str_field(r, "description");
    const auto forbidden = check_forbidden_rule(id, description, instructions);
    if (!forbidden.ok) return log_err(args, "add_rule forbidden: " + forbidden.reason);

    // 既存 (= seed 含む) の id は再 insert しない (= ignore)
    if (args.rules->get(id)) return log_err(args, "add_rule id already exists: " + id);

    RuleSeed seed;
    seed.id = id;
    seed.description = description;
    seed.trigger_type = trigger;
    if (auto t = num_field(r, "tick_sec"); t && *t >= 5 && *t <= 3600) seed.tick_sec = static_cast<std::int64_t>(*t);
    seed.event_kind = str_field(r, "event_kind");
    seed.target = str_field(r, "target");
    if (auto c = num_field(r, "cooldown_sec"); c && *c >= 0 && *c <= 86400)
        seed.cooldown_sec = static_cast<std::int64_t>(*c);
    else
        seed.cooldown_sec = 60;
    seed.instructions = instructions;

    args.rules->insert_or_ignore(seed, "ai:" + args.personaId);
    log_rule(args, id, "add", "persona " + args.personaId + " added rule " + id + " via " + args.ruleId);
    args.logger->info(Json{{"rule_id", id}, {"persona_id", args.personaId}, {"via", args.ruleId}}, "rule added by AI");
    return {"add_rule", id, id};
}

HandleActionResult handle_remove_rule(const HandleActionArgs& args, const Json& json) {
    const std::string rule_id = str_field(json, "rule_id").value_or("");
    const std::string reasoning = str_field(json, "reasoning").value_or("(no reason)");
    if (rule_id.empty()) return log_err(args, "remove_rule without rule_id");
    if (seeds::SEED_RULE_IDS().count(rule_id)) return log_err(args, "remove_rule rejected: " + rule_id + " is a protected seed");
    if (!args.rules->get(rule_id)) return log_err(args, "remove_rule unknown id: " + rule_id);
    args.rules->remove(rule_id, "ai:" + args.personaId, reasoning);
    log_rule(args, rule_id, "remove", "persona " + args.personaId + " removed rule " + rule_id + ": " + reasoning);
    args.logger->info(Json{{"rule_id", rule_id}, {"persona_id", args.personaId}, {"reasoning", reasoning}}, "rule removed by AI");
    return {"remove_rule", rule_id, std::nullopt};
}

std::optional<std::string> non_empty(std::optional<std::string> v) {
    if (v && v->empty()) return std::nullopt;
    return v;
}

} // namespace

JsonLimits action_json_limits() { return JsonLimits{1u << 20, 50'000, 32}; }

ForbiddenRuleCheck check_forbidden_rule(const std::string& id, const std::optional<std::string>& description,
                                        const std::string& instructions) {
    const std::string blob = str::to_lower_ascii(id + "\n" + description.value_or("") + "\n" + instructions);
    for (const auto& p : forbidden_patterns())
        for (const auto& seq : p.alternatives)
            if (search(blob, seq)) return {false, p.reason};
    return {true, {}};
}

std::optional<Json> extract_json(const std::string& text) {
    const std::string trimmed = str::trim(text);
    const auto limits = action_json_limits();
    if (auto v = parse_json(trimmed, limits); is_object(v)) return v;
    // try to find first { ... } balanced
    const std::size_t start = trimmed.find('{');
    if (start == std::string::npos) return std::nullopt;
    long depth = 0;
    int attempts = 0;
    for (std::size_t i = start; i < trimmed.size(); ++i) {
        const char c = trimmed[i];
        if (c == '{') {
            ++depth;
        } else if (c == '}') {
            --depth;
            if (depth == 0) {
                if (++attempts > kMaxExtractAttempts) return std::nullopt;
                if (auto v = parse_json(std::string_view(trimmed).substr(start, i + 1 - start), limits); is_object(v)) return v;
            }
        }
    }
    return std::nullopt;
}

HandleActionResult handle_action(const HandleActionArgs& args) {
    const auto parsed = extract_json(args.rawText);
    if (!parsed) {
        const std::string detail = "invalid JSON: " + str::js_slice(args.rawText, 0, 80);
        log_rule(args, args.ruleId, "error", detail);
        args.logger->warn(Json{{"rule_id", args.ruleId}}, detail);
        return {"error", detail, std::nullopt};
    }
    const Json& json = *parsed;

    const std::string raw_action = str_field(json, "action").value_or("");
    // LLM が post_utterance を speak/say/utterance 等の別名で返すことがあるので
    // 発話系の同義語は post_utterance に正規化する (text があれば発話とみなす)。
    static const std::set<std::string> kSpeakAliases = {"speak", "say", "utter", "utterance", "message", "talk", "reply"};
    const std::string action =
        kSpeakAliases.count(raw_action) && str_field(json, "text") ? std::string("post_utterance") : raw_action;

    if (action == "skip") {
        const std::string reasoning = str_field(json, "reasoning").value_or("(no reason)");
        args.rules->log({args.ruleId, "skip", "ai", "persona " + args.personaId + " skipped: " + reasoning});
        return {"skip", reasoning, std::nullopt};
    }

    if (action == "propose_hypothesis") {
        const auto statement = str_field(json, "statement");
        if (!statement || str::trim(*statement).empty()) return log_err(args, "propose_hypothesis without statement");
        ProposeHypothesisInput input;
        input.workspaceId = args.workspaceId;
        input.statement = *statement;
        if (auto a = str_field(json, "addresses_gap_id"))
            input.designGapId = a;
        else if (auto g = str_field(json, "gap_id"))
            input.designGapId = g;
        else
            input.designGapId = infer_design_gap_id_from_trigger(args.triggerEvent);
        input.byPersonaId = args.personaId;
        input.reasoning = str_field(json, "reasoning");
        const std::string id = args.contextProvider->propose_hypothesis(input);
        log_rule(args, args.ruleId, "fire", "persona " + args.personaId + " proposed hypothesis " + id);
        args.logger->info(Json{{"rule_id", args.ruleId}, {"persona_id", args.personaId}, {"hypothesis_id", id}},
                          "hypothesis proposed");
        return {"propose_hypothesis", input.statement, id};
    }

    if (action == "add_rule") return handle_add_rule(args, json);
    if (action == "remove_rule") return handle_remove_rule(args, json);

    if (action == "post_utterance") {
        const auto text = str_field(json, "text");
        if (!text || str::trim(*text).empty()) return log_err(args, "post_utterance without text");
        // tick rule は session context を持たない。json.hypothesis_id から紐付く議論 session を解決する。
        // hypothesis も無い純 debate 発話は「進行中の discord 議論」にフォールバックする (無ければ skip)。
        std::optional<std::string> session_id = non_empty(args.sessionId);
        if (!session_id) {
            const auto hypothesis_id = non_empty(str_field(json, "hypothesis_id"));
            if (hypothesis_id) {
                FindDiscussionSessionInput q;
                q.workspaceId = args.workspaceId;
                q.hypothesisId = hypothesis_id;
                session_id = non_empty(args.contextProvider->find_discussion_session(q));
            }
            if (!session_id) session_id = non_empty(args.contextProvider->find_active_discussion_session(args.workspaceId));
        }
        if (!session_id)
            return {"skip", "post_utterance: 議論 session を解決できないため skip (tick rule)", std::nullopt};
        PostUtteranceInput input;
        input.workspaceId = args.workspaceId;
        input.sessionId = *session_id;
        input.text = *text;
        input.byPersonaId = args.personaId;
        input.respondsTo = str_field(json, "responds_to");
        const std::string id = args.contextProvider->post_utterance(input);
        log_rule(args, args.ruleId, "fire", "persona " + args.personaId + " posted utterance " + id);
        args.logger->info(Json{{"rule_id", args.ruleId}, {"persona_id", args.personaId}, {"utterance_id", id}},
                          "utterance posted");
        return {"post_utterance", input.text, id};
    }

    return log_err(args, "unknown action: " + action);
}

} // namespace di::persona_engine::engine
