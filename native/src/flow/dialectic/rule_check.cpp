#include "flow/dialectic/rule_check.hpp"

#include "base/strings.hpp"

namespace di::flow::dialectic {

std::string rule_foul_label(const RuleFoul& foul) {
    if (foul == "incomplete") return "反論自身のデータか論拠が示されていない";
    if (foul == "unanchored") return "どの根拠のデータ・論拠・限定を突くのかが示されていない";
    if (foul == "out_of_scope") return "相手の主張の限定の外側から全体を否定している";
    if (foul == "straw_man") return "相手が言っていない主張に置き換えている";
    if (foul == "dismissal") return "根拠の中身に触れずに退けている";
    if (foul == "topic_shift") return "論点をずらしている";
    if (foul == "self_contradiction") return "自分の陣営の主張と逆のこと (相手陣営の主張を支える内容) を述べている";
    if (foul == "unsupported_attribution") return "本人の発言だと言っているのに、根拠にした本人の発言が示されていない";
    if (foul == "quote_mismatch") return "本人の発言の引用だと言っているのに、その言葉が本人の発言に無い";
    return "";
}

AnchorResult check_rebut_anchor(const RebutClaim& claim, const PositionRecord& position) {
    AnchorResult r;
    if (!claim.data || claim.data->empty() || !claim.warrant || claim.warrant->empty()) {
        r.foul = "incomplete";
        return r;
    }
    if (!claim.attack || claim.attack->empty()) {
        r.foul = "unanchored";
        return r;
    }
    if (*claim.attack == "qualifier") {
        r.ok = true;
        return r;
    }
    if (claim.groundId && !claim.groundId->empty()) {
        for (const auto& g : position.grounds) {
            if (g.id == *claim.groundId && g.state != "conceded") {
                r.ok = true;
                r.ground = g;
                return r;
            }
        }
    }
    r.foul = "unanchored";
    return r;
}

namespace {

std::string render_target(const PositionRecord& position, const std::optional<Ground>& ground) {
    std::vector<std::string> lines{"主張: " + position.claim, "限定: " + position.qualifier.value_or("(明示なし)")};
    if (ground) {
        lines.push_back("突かれた根拠 [" + ground->id + "]" + (ground->kind ? " (" + inference_kind_label(*ground->kind) + ")" : ""));
        lines.push_back("  データ: " + ground->text);
        lines.push_back("  論拠: " + ground->warrant.value_or("(明示なし)"));
    }
    return str::join(lines, "\n");
}

} // namespace

std::string build_rule_judge_prompt(const RuleJudgeInput& a) {
    const bool own = a.ownPosition.has_value();
    return "# 論点\n" + a.issue.title + "\n\n" + "# 反論された側\n" + render_target(a.position, a.ground) + "\n\n" +
           (own ? "# 反論した側の陣営の主張 (反論はこの主張を守る/支える向きでなければならない)\n" +
                      ("主張: " + a.ownPosition->claim + "\n限定: " + a.ownPosition->qualifier.value_or("(明示なし)") + "\n\n")
                : std::string()) +
           "# 反論 (「" + attack_point_label(a.attack) + "」を突くと宣言)\n" + a.rebutText + "\n" + "  反論のデータ: " +
           a.rebutData + "\n" + "  反論の論拠: " + a.rebutWarrant + "\n\n" +
           "この反論が議論の前提ルールを守っているかを判定し、ラベルを 1 つだけ返してください (説明不要):\n" +
           "- ok: 宣言した要素 (データの真偽・論拠の妥当性・限定の範囲) を、相手の主張の限定の内側で突いている\n" +
           "- out_of_scope: 相手の限定の外側にある層・条件・事例を持ち出して主張全体を否定している\n" +
           "- straw_man: 相手が言っていない主張に置き換えて否定している\n" +
           "- dismissal: 根拠の中身に触れず「感想にすぎない」「データが無い」だけで退けている (何が足りないかを具体的に示していない)\n" +
           "- topic_shift: この論点と別の論点に話をずらしている\n" +
           (own ? std::string("- self_contradiction: 反論の内容が、反論した側の陣営の主張と逆向きになっている (相手陣営の主張を支えてしまっている)\n")
                : std::string()) +
           "判断に迷う場合は ok を返す。";
}

std::optional<std::string> parse_rule_verdict(const std::string& text) {
    const std::string lower = str::to_lower(text);
    std::optional<std::string> best;
    std::size_t best_index = std::string::npos;
    for (const char* label : {"out_of_scope", "straw_man", "dismissal", "topic_shift", "self_contradiction", "ok"}) {
        const auto idx = lower.find(label);
        if (idx != std::string::npos && (!best || idx < best_index)) {
            best = label;
            best_index = idx;
        }
    }
    return best;
}

std::string judge_rule_violation(persona_engine::LLMClient& llm, const RuleJudgeInput& input,
                                 const std::optional<std::string>& model, const WarnFn& warn) {
    persona_engine::LLMInvokeArgs call;
    call.prompt = build_rule_judge_prompt(input);
    if (model && !model->empty()) call.model = *model;
    call.maxTokens = 50;
    const auto result = llm.invoke(call);
    if (!result.ok) {
        if (warn) warn("前提ルール判定 LLM エラー: " + result.error + " — ok として続行 (degrade)");
        return "ok";
    }
    auto verdict = parse_rule_verdict(result.text);
    if (!verdict) {
        if (warn) warn("前提ルール判定ラベル不明: \"" + str::js_slice(result.text, 0, 60) + "\" — ok として続行 (degrade)");
        return "ok";
    }
    return *verdict;
}

RebutGateResult gate_rebut(persona_engine::LLMClient& llm, const GateRebutArgs& args) {
    RebutGateResult r;
    const auto anchor = check_rebut_anchor(args.claim, args.position);
    if (!anchor.ok) {
        r.foul = anchor.foul;
        return r;
    }
    if (!args.judge || !args.claim.attack || !args.claim.data || !args.claim.warrant) {
        r.accepted = true;
        r.ground = anchor.ground;
        return r;
    }
    const std::string verdict = judge_rule_violation(
        llm, {args.issue, args.position, args.ownPosition, anchor.ground, *args.claim.attack, args.rebutText, *args.claim.data,
              *args.claim.warrant},
        args.model, args.warn);
    if (verdict == "ok") {
        r.accepted = true;
        r.ground = anchor.ground;
    } else {
        r.foul = verdict;
    }
    return r;
}

std::string build_concession_prompt(const std::string& target_text, const std::string& reply_text) {
    return "# 相手の発言\n" + target_text + "\n\n# 返答\n" + reply_text + "\n\n" +
           "返答は相手の発言を認めているか、それとも反論・追撃しているかを判定し、ラベルを 1 つだけ返してください (説明不要):\n" +
           "- concede: 相手の言い分を認めている (部分的に認めて条件を付けるのも含む)\n" +
           "- rebut: 相手の言い分を否定・反論・追撃している\n" + "判断に迷う場合は concede を返す。";
}

std::string judge_concession(persona_engine::LLMClient& llm, const std::string& target_text, const std::string& reply_text,
                             const std::optional<std::string>& model, const WarnFn& warn) {
    persona_engine::LLMInvokeArgs call;
    call.prompt = build_concession_prompt(target_text, reply_text);
    if (model && !model->empty()) call.model = *model;
    call.maxTokens = 50;
    const auto result = llm.invoke(call);
    if (!result.ok) {
        if (warn) warn("認めるの判定 LLM エラー: " + result.error + " — 申告どおり concede として続行 (degrade)");
        return "concede";
    }
    const std::string lower = str::to_lower(result.text);
    const auto r = lower.find("rebut");
    const auto c = lower.find("concede");
    if (r == std::string::npos && c == std::string::npos) {
        if (warn)
            warn("認めるの判定ラベル不明: \"" + str::js_slice(result.text, 0, 60) + "\" — 申告どおり concede として続行 (degrade)");
        return "concede";
    }
    return (r != std::string::npos && (c == std::string::npos || r < c)) ? "rebut" : "concede";
}

std::string render_foul_notice(const std::string& persona_name, const RuleFoul& foul) {
    if (foul == "unsupported_attribution" || foul == "quote_mismatch")
        return persona_name + "さんの今の反論は「" + rule_foul_label(foul) + "」ので、本人の考えとしては数えずに問いとして扱いますね。" +
               "本人の発言 [E番号] に支えられることだけを本人の考えとして言い、それ以外は推測として言ってください。";
    return persona_name + "さんの今の反論は「" + rule_foul_label(foul) + "」ので、根拠への反論としては数えずに問いとして扱いますね。" +
           "反論のデータと論拠を示し、相手の主張の範囲の中でデータ・論拠・限定のどれが違うのかを突いてもらえると議論が進みます。";
}

} // namespace di::flow::dialectic
