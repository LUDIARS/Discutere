#include "flow/persona_setup.hpp"

#include <map>
#include <stdexcept>

#include "base/log.hpp"
#include "base/strings.hpp"
#include "base/time.hpp"
#include "flow/cost_logger.hpp"
#include "flow/db/connection.hpp"

namespace di::flow {

namespace {

struct PersonaValueAssignment {
    std::string personaId;
    std::optional<std::string> valueAxis;
    std::optional<std::vector<std::string>> coreClaims;
};

/** LLM 応答テキストから最初の '[' 〜 最後の ']' を JSON 配列として読む (壊れていれば throw)。 */
Json extract_json_array_strict(std::string_view text) {
    const auto start = text.find('[');
    const auto end = text.rfind(']');
    if (start == std::string_view::npos || end == std::string_view::npos || end <= start)
        throw std::runtime_error("no JSON array in LLM output");
    std::string error;
    auto parsed = parse_json(text.substr(start, end - start + 1), llm_json_limits(), &error);
    if (!parsed) throw std::runtime_error(error.empty() ? "invalid JSON" : error);
    if (!parsed->is_array()) throw std::runtime_error("LLM output is not a JSON array");
    return *parsed;
}

std::vector<PersonaValueAssignment> coerce_assignments(const Json& raw) {
    std::vector<PersonaValueAssignment> out;
    for (const auto& item : raw) {
        if (!item.is_object()) continue;
        const auto id = item.find("personaId");
        if (id == item.end() || !id->is_string() || id->get<std::string>().empty()) continue;
        PersonaValueAssignment a;
        a.personaId = id->get<std::string>();
        const auto axis = item.find("valueAxis");
        if (axis != item.end() && axis->is_string() && !str::trim(axis->get<std::string>()).empty())
            a.valueAxis = str::trim(axis->get<std::string>());
        const auto claims = item.find("coreClaims");
        if (claims != item.end() && claims->is_array()) {
            std::vector<std::string> list;
            for (const auto& c : *claims) {
                if (!c.is_string() || str::trim(c.get<std::string>()).empty()) continue;
                list.push_back(c.get<std::string>());
                if (list.size() >= 2) break;
            }
            a.coreClaims = std::move(list);
        }
        out.push_back(std::move(a));
    }
    return out;
}

} // namespace

std::string build_persona_setup_prompt(const std::string& theme, const std::vector<FlowPersona>& personas) {
    std::vector<std::string> cast;
    for (const auto& p : personas)
        cast.push_back("- id: " + p.id + " / 名前: " + p.name + " / ロール: " + p.role + " / スタンス: " + p.stance);
    return "テーマ「" + theme + "」の議論に参加するペルソナ一覧です。\n" +
           "各ペルソナに「重視する価値 (価値軸)」を 1 文で割り当ててください。\n" +
           "debater には加えて「核となる主張」を 1〜2 個割り当ててください\n" +
           "(pro と con は対立が成立するよう主張を対にする。facilitator / opinion は coreClaims 省略可)。\n\n" +
           "# ペルソナ\n" + str::join(cast, "\n") + "\n\n" + "返答は次の JSON 配列のみ (説明不要):\n" +
           "[{ \"personaId\": \"<id>\", \"valueAxis\": \"<重視する価値 1 文>\", \"coreClaims\": [\"<核主張>\"] }]";
}

void persist_session_personas(const std::string& session_id, const std::vector<FlowPersona>& personas) {
    auto db = db::get_flow_db();
    const auto now = time::now_ms();
    for (const auto& p : personas) {
        db->run(R"SQL(INSERT OR REPLACE INTO flow_session_persona
       (id, session_id, name, role, stance, value_axis, core_claims_json, possession_name, created_at)
     VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?))SQL",
                {p.id, session_id, p.name, p.role, p.stance, sql::Value(p.valueAxis),
                 p.coreClaims ? sql::Value(json_stringify(Json(*p.coreClaims))) : sql::Value(),
                 p.possession ? sql::Value(p.possession->label) : sql::Value(), now});
    }
}

std::vector<FlowPersona> setup_session_personas(persona_engine::LLMClient& llm, const SetupSessionPersonasArgs& args) {
    const WarnFn warn = args.warn ? args.warn : WarnFn([](const std::string& m) { log::warn(m); });
    auto logged = with_cost_log(llm, {args.flow, args.sessionId, {}, {}, {}, {}, "persona-setup", {}});

    std::vector<FlowPersona> enriched = args.personas;
    persona_engine::LLMInvokeArgs call;
    call.prompt = build_persona_setup_prompt(args.theme, args.personas);
    if (args.model && !args.model->empty()) call.model = *args.model;
    const auto result = logged.invoke(call);

    if (!result.ok) {
        warn("ペルソナ価値軸生成 失敗: " + result.error + " — 価値軸なしで議論を続行 (degrade)");
    } else {
        try {
            std::map<std::string, PersonaValueAssignment> by_id;
            for (auto& a : coerce_assignments(extract_json_array_strict(result.text))) by_id[a.personaId] = a;
            for (auto& p : enriched) {
                auto it = by_id.find(p.id);
                if (it == by_id.end()) continue;
                if (it->second.valueAxis) p.valueAxis = it->second.valueAxis;
                // coreClaims は debater のみ採用 (spec 01: 他ロールは注入しない)。
                if (p.role == "debater" && it->second.coreClaims && !it->second.coreClaims->empty())
                    p.coreClaims = it->second.coreClaims;
            }
        } catch (const std::exception& e) {
            enriched = args.personas;
            warn(std::string("ペルソナ価値軸生成 JSON パース失敗: ") + e.what() + " — 価値軸なしで議論を続行 (degrade)");
        }
    }
    persist_session_personas(args.sessionId, enriched);
    return enriched;
}

} // namespace di::flow
