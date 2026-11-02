#include "flow/dialectic/exchange_prompt.hpp"

#include "base/strings.hpp"
#include "flow/dialectic/turn_prompt.hpp"
#include "flow/discussion_paper.hpp"
#include "flow/proxy/prompt.hpp"

namespace di::flow::dialectic {

namespace {

std::string render_node(const ArgumentNode& n, const std::map<std::string, std::string>& names, const std::string& self_id) {
    auto it = names.find(n.personaId);
    const std::string who = it == names.end() ? n.personaId : it->second;
    const std::string owner = n.personaId == self_id ? who + " (あなた)" : who;
    const std::string kind = n.kind ? " (" + inference_kind_label(*n.kind) + ")" : "";
    const std::string label = n.depth == 0 ? "論拠 [" + n.groundId.value_or(n.id) + "]" : "反論 " + std::to_string(n.depth);
    const std::string warrant = (n.warrant && !n.warrant->empty()) ? "\n    論拠: " + *n.warrant : "";
    return "- " + label + " " + owner + kind + "\n    データ: " + n.data + warrant + "\n    発言: " + n.text;
}

std::optional<std::string> str_field(const Json& o, const char* key) {
    auto it = o.find(key);
    if (it == o.end() || !it->is_string()) return std::nullopt;
    auto t = str::trim(it->get<std::string>());
    if (t.empty()) return std::nullopt;
    return t;
}

} // namespace

std::string build_exchange_prompt(const ExchangePromptArgs& args) {
    const auto& persona = args.persona;
    const ArgumentNode* target = args.path.empty() ? nullptr : &args.path.back();
    auto name_of = [&](const std::string& id) {
        auto it = args.personaNames.find(id);
        return it == args.personaNames.end() ? id : it->second;
    };
    std::vector<std::string> blocks;
    for (const auto& p : args.positions) {
        const std::string name = name_of(p.personaId);
        blocks.push_back(render_position_digest(p, p.personaId == persona.id ? name + " (あなた)" : name));
    }
    const std::string position_block = str::join(blocks, "\n\n");
    std::vector<std::string> chain;
    for (const auto& n : args.path) chain.push_back(render_node(n, args.personaNames, persona.id));
    const std::string target_name = target ? name_of(target->personaId) : "相手";
    const PositionRecord* own = nullptr;
    for (const auto& p : args.positions)
        if (p.stance == args.stance) {
            own = &p;
            break;
        }
    const std::string side_ja = args.stance == "pro" ? "賛成" : args.stance == "con" ? "反対" : "";

    std::vector<std::string> lines{"あなたは議論ペルソナ「" + persona.name + "」。",
                                   "特徴: " + str::join(persona.traits, " / ") + " / 話し方: " + persona.speechStyle};
    if (persona.valueAxis && !persona.valueAxis->empty()) lines.push_back("あなたが重視する価値: " + *persona.valueAxis);
    lines.push_back(stance_line(args.stance));
    lines.emplace_back("");
    for (auto& l : proxy::proxy_prompt_lines(persona)) lines.push_back(std::move(l));
    lines.push_back(premise_rules_text());
    lines.emplace_back("");
    lines.push_back("# 論点 " + std::to_string(args.issue.ordinal));
    lines.push_back(args.issue.title);
    lines.emplace_back("");
    if (args.voicesBlock && !args.voicesBlock->empty()) {
        lines.push_back(*args.voicesBlock);
        lines.emplace_back("");
    }
    lines.emplace_back("# 両陣営の主張と論拠");
    lines.push_back(position_block.empty() ? "(Position なし)" : position_block);
    lines.emplace_back("");
    lines.emplace_back("# この論拠の応酬 (上から順に。最後が応答対象)");
    lines.push_back(str::join(chain, "\n"));
    lines.emplace_back("");
    if (own) {
        // 発言のたびに自分の立場を明示する。
        lines.emplace_back("# あなたの立場 (忘れないこと)");
        lines.push_back("あなたは【" + side_ja + "】陣営。あなたの陣営の主張: " + own->claim +
                        ((own->qualifier && !own->qualifier->empty()) ? " (限定: " + *own->qualifier + ")" : ""));
        lines.emplace_back("答えは必ずこの主張を守る向き (相手の論拠を崩す / 自分の陣営の論拠を守る) にする。");
        lines.emplace_back("相手陣営の主張を支える内容を、反論として言わない。相手の言い分が正しいと思うなら反論せず concede で認める。");
        lines.emplace_back("");
    }
    lines.emplace_back("# 指示");
    lines.push_back(target_name + "さんの最後の" + (target && target->depth > 0 ? "反論" : "論拠") +
                    "に答えてください。答え方は次のどちらか 1 つ:");
    lines.emplace_back("- rebut: 反論する。反論も論拠 1 つだけで立てる (データ 1 つ + 論拠 1 つ)");
    lines.emplace_back("- concede: 相手の言い分を認める (崩せないと思うなら認める)");
    lines.emplace_back("次の JSON 1 個だけを返す (前後に説明やコードフェンスを付けない):");
    lines.push_back(std::string("{\"act\": \"<rebut | concede>\", \"attack\": \"<rebut のとき必須: data | warrant | qualifier (相手のデータ・論拠・限定のどれを突くか)>\", ") +
                    "\"kind\": \"<rebut のとき: deduction | induction | abduction>\", " +
                    "\"data\": \"<rebut のとき必須: 反論のデータ (事実・事例・仕様) 1 つ>\", " +
                    "\"warrant\": \"<rebut のとき必須: 反論の論拠 (そのデータで相手のどこが崩れるか) 1 つ>\", " +
                    "\"text\": \"<Discord に流す口語 1〜3 文>\"" + proxy::proxy_ground_fields(persona) + "}");
    lines.emplace_back("反論は相手の主張の限定の内側で突く。論拠を 2 つ以上並べない。");
    lines.push_back(std::string("根拠にする声は、プレイヤーの声の中から複数を照らし合わせて選ぶ。続けている人の声だけで全体を語らず、") +
                    "やめた人・不満のある人の声があればそれも検討する。声が足りなければ、何の材料が足りないかを言う。");
    lines.emplace_back("text は実在の人間の雑談のような自然な口語で書く (ラベル・箇条書き禁止)。");
    return str::join(lines, "\n");
}

Json to_json(const ParsedExchangeTurn& t) {
    auto opt = [](const std::optional<std::string>& v) { return v ? Json(*v) : Json(nullptr); };
    Json o = Json::object();
    o["act"] = t.act;
    o["attack"] = opt(t.attack);
    o["kind"] = opt(t.kind);
    o["data"] = opt(t.data);
    o["warrant"] = opt(t.warrant);
    o["text"] = t.text;
    o["degraded"] = t.degraded;
    o["claimKind"] = opt(t.claimKind);
    o["evidenceIds"] = t.evidenceIds;
    return o;
}

ParsedExchangeTurn parse_exchange_response(const std::string& raw, const WarnFn& warn) {
    const auto obj = extract_json_object(raw);
    const auto text = obj ? str_field(*obj, "text") : std::nullopt;
    if (!obj || !text) {
        if (warn) warn("応酬の応答 JSON パース失敗 — question として受理 (degrade): " + str::js_slice(str::trim(raw), 0, 60));
        ParsedExchangeTurn t;
        t.act = "question";
        t.text = str::trim(raw);
        t.degraded = true;
        return t;
    }
    const auto raw_act = str_field(*obj, "act");
    const std::string lower = raw_act ? str::to_lower(*raw_act) : "";
    if (lower != "concede" && lower != "rebut" && warn) warn("応酬の act \"" + lower + "\" は不明 — rebut として扱う");
    auto get = [&](const char* key) { return obj->contains(key) ? (*obj)[key] : Json(); };
    ParsedExchangeTurn t;
    t.act = lower == "concede" ? "concede" : "rebut";
    t.attack = coerce_attack_point(get("attack"));
    t.kind = coerce_inference_kind(get("kind"));
    t.data = str_field(*obj, "data");
    t.warrant = str_field(*obj, "warrant");
    t.text = *text;
    t.degraded = false;
    t.claimKind = proxy::coerce_claim_kind(get("claimKind"));
    t.evidenceIds = proxy::coerce_evidence_ids(get("evidenceIds"));
    return t;
}

} // namespace di::flow::dialectic
