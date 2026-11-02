#include "flow/personas.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

#include "base/crypto.hpp"
#include "base/strings.hpp"

namespace di::flow {

namespace {

// 人格プール (composition.ts と同じプールを流用)
const std::vector<std::string>& name_pool() {
    static const std::vector<std::string> pool = {
        "陽介", "凛", "ハル", "美咲", "翔", "葵", "蓮", "結衣", "拓海", "彩",
        "悠真", "詩織", "大樹", "舞", "颯太", "真央", "啓介", "杏奈", "亮", "千夏",
    };
    return pool;
}

const std::vector<std::string>& surname_pool() {
    static const std::vector<std::string> pool = {
        "佐伯", "三上", "高瀬", "橘", "藤堂", "水野", "神谷", "篠原", "久我", "朝倉",
        "一条", "白石", "桐生", "成瀬", "榊", "相馬", "黒川", "有馬", "二宮", "望月",
    };
    return pool;
}

std::string to_full_name(const std::string& given_name, std::size_t ordinal) {
    const std::string name = str::trim(given_name);
    const auto& surnames = surname_pool();
    if (name.empty()) return surnames[ordinal % surnames.size()] + " 名称未設定";
    if (name.find(' ') != std::string::npos || name.find("　") != std::string::npos) return name;
    return surnames[ordinal % surnames.size()] + " " + name;
}

struct Flavor {
    const char* style;
    std::vector<std::string> traits;
};

const Flavor& role_flavor(const FlowRole& role) {
    static const Flavor facilitator{"穏やかな進行役口調", {"中立", "俯瞰", "巻き込み上手"}};
    static const Flavor debater{"芯のある主張口調", {"論点を掴む", "具体例豊富", "対立を恐れない"}};
    static const Flavor opinion{"自然な雑談口調", {"独自の角度", "素直", "発想が柔らかい"}};
    if (role == "facilitator") return facilitator;
    if (role == "debater") return debater;
    return opinion;
}

// Math.floor(rng() * n) を [0, n) に収める (rng が 1 以上を返しても範囲外を引かない)。
std::size_t pick_index(double r, std::size_t n) {
    if (n == 0) return 0;
    double f = std::floor(r * static_cast<double>(n));
    if (!(f >= 0)) f = 0;
    if (f >= static_cast<double>(n)) f = static_cast<double>(n - 1);
    return static_cast<std::size_t>(f);
}

std::string pick_name(std::set<std::string>& used, std::size_t& used_size, const Rng& rng) {
    const auto& full = full_name_pool();
    std::vector<std::string> free;
    for (const auto& n : full)
        if (!used.count(n)) free.push_back(n);
    const auto& pool = free.empty() ? full : free;
    const std::string name = pool[pick_index(rng(), pool.size())];
    const std::string final_name = used.count(name) ? name + std::to_string(used_size) : name;
    if (used.insert(final_name).second) ++used_size;
    return final_name;
}

FlowPersona make(const std::string& name, const FlowRole& role, const FlowStance& stance, const std::string& model,
                 bool is_local) {
    const Flavor& f = role_flavor(role);
    FlowPersona p;
    p.id = crypto::uuid_v4();
    p.name = name;
    p.role = role;
    p.stance = stance;
    p.speechStyle = f.style;
    p.traits = f.traits;
    p.model = model;
    p.isLocal = is_local;
    return p;
}

std::optional<std::string> opt_str(const Json& j, const char* key) { return json_opt_string(j, key); }

std::vector<std::string> str_list(const Json& j) {
    std::vector<std::string> out;
    if (!j.is_array()) return out;
    for (const auto& v : j)
        if (v.is_string()) out.push_back(v.get<std::string>());
    return out;
}

} // namespace

const std::vector<std::string>& full_name_pool() {
    static const std::vector<std::string> pool = [] {
        std::vector<std::string> out;
        const auto& names = name_pool();
        for (std::size_t i = 0; i < names.size(); ++i) out.push_back(to_full_name(names[i], i));
        return out;
    }();
    return pool;
}

FlowStance role_default_stance(const FlowRole& role) {
    if (role == "facilitator") return "neutral";
    if (role == "opinion") return "opinion";
    return "pro";
}

FlowStance persona_stance(const FlowPersona& persona) {
    if (persona.role == "facilitator") return "neutral";
    if (persona.role == "opinion") return "opinion";
    return persona.stance == "pro" || persona.stance == "con" ? persona.stance : role_default_stance(persona.role);
}

int roster_persona_count(int persona_count, const std::optional<FlowModelRoster>& roster) {
    const std::size_t n = roster ? roster->discussants.size() : 0;
    return n > 0 ? 1 + static_cast<int>(n) : persona_count;
}

std::vector<FlowPersona> generate_flow_personas(const GeneratePersonasArgs& args) {
    if (args.count < 1) throw std::invalid_argument("personaCount must be >= 1");
    const Rng& rng = args.rng;
    std::set<std::string> used;
    std::size_t used_size = 0;
    std::vector<FlowPersona> personas;
    const std::string facilitator_model =
        args.roster && args.roster->facilitator && !args.roster->facilitator->empty() ? *args.roster->facilitator
                                                                                     : args.defaultModel;
    const std::vector<std::string> discussant_models = args.roster ? args.roster->discussants : std::vector<std::string>{};
    // 議論者のモデルは編成表を先頭から順に (足りなければ巡回) 割り当てる。
    auto discussant_model = [&](std::size_t index) {
        return discussant_models.empty() ? args.defaultModel : discussant_models[index % discussant_models.size()];
    };
    std::size_t discussant_index = 0;

    // 1 人目はファシリテーター
    personas.push_back(make(pick_name(used, used_size, rng), "facilitator", "neutral", facilitator_model, args.isLocal));

    // 残りの人数を opinion と debater に 1:2 で配分
    const int remaining = args.count - 1;
    const int opinion_count = remaining > 0 ? std::max(1, remaining / 3) : 0;
    const int debater_count = remaining - opinion_count;
    for (int i = 0; i < opinion_count; ++i)
        personas.push_back(make(pick_name(used, used_size, rng), "opinion", "opinion", discussant_model(discussant_index++),
                                args.isLocal));

    // debater は pro/con に均等割。奇数なら rng で余り 1 を pro/con のどちらかに振る。
    const int pro_count = debater_count / 2 + (debater_count % 2 == 1 && rng() < 0.5 ? 1 : 0);
    for (int i = 0; i < debater_count; ++i)
        personas.push_back(make(pick_name(used, used_size, rng), "debater", i < pro_count ? "pro" : "con",
                                discussant_model(discussant_index++), args.isLocal));
    return personas;
}

const FlowPersona& pick_random_persona(const std::vector<FlowPersona>& personas, const Rng& rng) {
    if (personas.empty()) throw std::out_of_range("no personas");
    return personas[pick_index(rng(), personas.size())];
}

Json to_json(const PersonaPossession& p) {
    Json o = Json::object();
    o["label"] = p.label;
    if (p.descriptor) o["descriptor"] = *p.descriptor;
    return o;
}

Json to_json(const ProxyPersonaContext& p) {
    Json texts = Json::object();
    for (const auto& [k, v] : p.evidenceTexts) texts[k] = v;
    Json o = Json::object();
    o["label"] = p.label;
    o["speakerKey"] = p.speakerKey;
    o["genre"] = p.genre;
    o["bundleText"] = p.bundleText;
    o["evidenceIds"] = p.evidenceIds;
    o["evidenceTexts"] = std::move(texts);
    return o;
}

Json to_json(const FlowPersona& p) {
    Json o = Json::object();
    o["id"] = p.id;
    o["name"] = p.name;
    o["role"] = p.role;
    o["stance"] = p.stance;
    o["speechStyle"] = p.speechStyle;
    o["traits"] = p.traits;
    o["model"] = p.model;
    o["isLocal"] = p.isLocal;
    if (p.valueAxis) o["valueAxis"] = *p.valueAxis;
    if (p.coreClaims) o["coreClaims"] = *p.coreClaims;
    if (p.possession) o["possession"] = to_json(*p.possession);
    if (p.proxy) o["proxy"] = to_json(*p.proxy);
    return o;
}

FlowPersona flow_persona_from_json(const Json& j) {
    FlowPersona p;
    if (!j.is_object()) return p;
    p.id = json_string(j, "id");
    p.name = json_string(j, "name");
    p.role = json_string(j, "role");
    p.stance = json_string(j, "stance");
    p.speechStyle = json_string(j, "speechStyle");
    p.traits = str_list(json_array(j, "traits"));
    p.model = json_string(j, "model");
    p.isLocal = json_bool(j, "isLocal");
    p.valueAxis = opt_str(j, "valueAxis");
    if (j.contains("coreClaims") && j["coreClaims"].is_array()) p.coreClaims = str_list(j["coreClaims"]);
    if (j.contains("possession") && j["possession"].is_object()) {
        const Json& po = j["possession"];
        p.possession = PersonaPossession{json_string(po, "label"), opt_str(po, "descriptor")};
    }
    if (j.contains("proxy") && j["proxy"].is_object()) {
        const Json& px = j["proxy"];
        ProxyPersonaContext c;
        c.label = json_string(px, "label");
        c.speakerKey = json_string(px, "speakerKey");
        c.genre = json_string(px, "genre");
        c.bundleText = json_string(px, "bundleText");
        c.evidenceIds = str_list(json_array(px, "evidenceIds"));
        const Json& texts = json_object(px, "evidenceTexts");
        for (auto it = texts.begin(); it != texts.end(); ++it)
            if (it.value().is_string()) c.evidenceTexts.emplace_back(it.key(), it.value().get<std::string>());
        p.proxy = std::move(c);
    }
    return p;
}

} // namespace di::flow
