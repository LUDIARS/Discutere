#include "flow/dialectic/position.hpp"

#include "base/strings.hpp"
#include "flow/proxy/prompt.hpp"

namespace di::flow::dialectic {

Json to_json(const GeneratedPosition& p) {
    Json o = Json::object();
    o["claim"] = p.claim;
    o["qualifier"] = p.qualifier ? Json(*p.qualifier) : Json(nullptr);
    Json grounds = Json::array();
    for (const auto& g : p.grounds) grounds.push_back(to_json(g));
    o["grounds"] = std::move(grounds);
    o["values"] = p.values;
    o["text"] = p.text;
    o["groundTexts"] = p.groundTexts;
    o["degraded"] = p.degraded;
    return o;
}

std::string build_position_prompt(const BuildPositionPromptArgs& args) {
    const auto& persona = args.persona;
    const auto& issue = args.issue;
    const std::string voices = (args.voicesBlock && !args.voicesBlock->empty()) ? *args.voicesBlock + "\n\n" : "";
    const std::string stance_ja = args.stance == "pro" ? "賛成" : "反対";
    const std::string seed = (persona.coreClaims && !persona.coreClaims->empty())
                                 ? "あなたの核となる主張 (これを種に組み立てる): " + str::join(*persona.coreClaims, " / ") + "\n"
                                 : "";
    return "あなたは議論ペルソナ「" + persona.name + "」。" + "特徴: " + str::join(persona.traits, " / ") +
           " / 話し方: " + persona.speechStyle + "\n" +
           ((persona.valueAxis && !persona.valueAxis->empty()) ? "あなたが重視する価値: " + *persona.valueAxis + "\n" : "") +
           seed + (persona.proxy ? "\n" + str::join(proxy::proxy_prompt_lines(persona), "\n") : "") + "\n# 論点 " +
           std::to_string(issue.ordinal) + "\n" + issue.title + "\n\n" + voices + premise_rules_text() + "\n\n" +
           "この論点についてあなたは【" + stance_ja + "】の立場 (" + args.stance + ") で定立 (Position) を張ります。\n" +
           "賛成 = 論点 (問い) に「はい」と答える立場、反対 = 「いいえ」と答える立場。" + "論点が問いの形でなければ「" +
           issue.title + " は成り立つか / 十分か」と読み替える。" + "主張は必ず【" + stance_ja +
           "】の向きにし、相手陣営より強い肯定・否定にすり替えない。\n" +
           ((args.feedback && !args.feedback->empty()) ? *args.feedback + "\n" : "") +
           "次の JSON 1 個だけを返してください (前後に説明やコードフェンスを付けない):\n" +
           "{\"claim\": \"<主張 1 文>\", \"qualifier\": \"<主張の限定: どのプレイヤー層・どの条件の話か>\", " +
           "\"grounds\": [{\"kind\": \"<deduction|induction|abduction>\", \"data\": \"<データ: 事実・事例・仕様>\", " +
           "\"warrant\": \"<論拠: データから主張が言える理由>\", \"say\": \"<この論拠を Discord に流す口語 1〜2 文>\"" +
           proxy::proxy_ground_fields(persona) + "}], " +
           "\"values\": [\"<価値前提 (例: 収益 > 体験)>\"], \"text\": \"<主張を Discord に流す口語 1〜2 文 (論拠は say で別に流す)>\"}\n" +
           "grounds は 1〜3 個 (強い論拠だけに絞る)。data と warrant は混ぜずに分ける。" +
           "text は実在の人間の雑談のような自然な口語で書く。";
}

std::string ground_id(const std::string& stance, std::size_t index) { return stance + "-G" + std::to_string(index + 1); }

namespace {

struct ParsedGround {
    std::string text;
    std::optional<InferenceKind> kind;
    std::optional<std::string> warrant;
    std::optional<std::string> say;
    std::optional<proxy::ClaimKind> claimKind;
    std::optional<std::vector<std::string>> evidenceIds;
};

std::optional<std::string> trimmed_string(const Json& o, const char* key) {
    auto it = o.find(key);
    if (it == o.end() || !it->is_string()) return std::nullopt;
    auto t = str::trim(it->get<std::string>());
    if (t.empty()) return std::nullopt;
    return t;
}

std::vector<std::string> coerce_string_array(const Json& v, std::size_t max) {
    std::vector<std::string> out;
    if (!v.is_array()) return out;
    for (const auto& s : v) {
        if (out.size() >= max) break;
        if (!s.is_string()) continue;
        auto t = str::trim(s.get<std::string>());
        if (!t.empty()) out.push_back(std::move(t));
    }
    return out;
}

/** grounds を正規化する。トゥールミン形式と旧形式 (文字列) の両方を受ける。 */
std::vector<ParsedGround> coerce_grounds(const Json& v, std::size_t max) {
    std::vector<ParsedGround> out;
    if (!v.is_array()) return out;
    for (const auto& item : v) {
        if (out.size() >= max) break;
        if (item.is_string()) {
            auto t = str::trim(item.get<std::string>());
            if (!t.empty()) out.push_back({t, {}, {}, {}, {}, {}});
            continue;
        }
        if (!item.is_object()) continue;
        std::string raw;
        if (item.contains("data") && item["data"].is_string()) raw = item["data"].get<std::string>();
        else if (item.contains("text") && item["text"].is_string()) raw = item["text"].get<std::string>();
        const std::string data = str::trim(raw);
        if (data.empty()) continue;
        ParsedGround g;
        g.text = data;
        g.kind = coerce_inference_kind(item.contains("kind") ? item["kind"] : Json());
        g.warrant = trimmed_string(item, "warrant");
        g.say = trimmed_string(item, "say");
        // 代理議論の申告 (検査は driver が proxy/attribution で行う)。
        g.claimKind = proxy::coerce_claim_kind(item.contains("claimKind") ? item["claimKind"] : Json());
        auto ids = proxy::coerce_evidence_ids(item.contains("evidenceIds") ? item["evidenceIds"] : Json());
        if (!ids.empty()) g.evidenceIds = std::move(ids);
        out.push_back(std::move(g));
    }
    return out;
}

} // namespace

GeneratedPosition generate_position(persona_engine::LLMClient& llm, const GeneratePositionArgs& args) {
    const auto& persona = args.persona;
    const auto& issue = args.issue;
    const auto& stance = args.stance;
    persona_engine::LLMInvokeArgs call;
    call.system = args.paperSystem;
    call.prompt = build_position_prompt({persona, stance, issue, args.voicesBlock, args.feedback});
    if (args.model && !args.model->empty()) call.model = *args.model;
    const auto result = llm.invoke(call);

    if (!result.ok) {
        // LLM 障害: coreClaims (respec 01) を機械的に Position 化して degrade。
        if (args.warn)
            args.warn("Position 生成 LLM エラー (" + persona.name + "/" + stance + "): " + result.error + " — coreClaims で degrade");
        const std::string claim = (persona.coreClaims && !persona.coreClaims->empty())
                                      ? (*persona.coreClaims)[0]
                                      : issue.title + " に" + (stance == "pro" ? "賛成" : "反対") + "の立場を取る";
        GeneratedPosition p;
        p.claim = claim;
        p.grounds.push_back({ground_id(stance, 0), claim, "unchallenged", {}, {}, {}, {}});
        if (persona.valueAxis && !persona.valueAxis->empty()) p.values.push_back(*persona.valueAxis);
        p.text = claim;
        p.groundTexts = {claim};
        p.degraded = true;
        return p;
    }

    const auto obj = extract_json_object(result.text);
    const auto claim_opt = obj ? trimmed_string(*obj, "claim") : std::nullopt;
    if (!claim_opt) {
        if (args.warn)
            args.warn("Position 生成 JSON パース失敗 (" + persona.name + "/" + stance + ") — 全文を claim として受理 (degrade)");
        const std::string text = str::trim(result.text);
        std::string head = str::js_slice(text, 0, 200);
        if (head.empty()) head = issue.title;
        GeneratedPosition p;
        p.claim = head;
        p.grounds.push_back({ground_id(stance, 0), head, "unchallenged", {}, {}, {}, {}});
        p.text = text.empty() ? issue.title : text;
        p.groundTexts = {head};
        p.degraded = true;
        return p;
    }

    const std::string claim = *claim_opt;
    auto parsed = coerce_grounds(obj->contains("grounds") ? (*obj)["grounds"] : Json(), GROUNDS_MAX);
    if (parsed.empty()) parsed.push_back({claim, {}, {}, {}, {}, {}});
    GeneratedPosition p;
    p.claim = claim;
    for (std::size_t i = 0; i < parsed.size(); ++i) {
        const auto& g = parsed[i];
        p.grounds.push_back({ground_id(stance, i), g.text, "unchallenged", g.kind, g.warrant, g.claimKind, g.evidenceIds});
        p.groundTexts.push_back(g.say ? *g.say : (g.warrant ? g.text + "。" + *g.warrant : g.text));
    }
    p.qualifier = trimmed_string(*obj, "qualifier");
    p.text = trimmed_string(*obj, "text").value_or(claim);
    p.values = coerce_string_array(obj->contains("values") ? (*obj)["values"] : Json(), 4);
    p.degraded = false;
    return p;
}

} // namespace di::flow::dialectic
