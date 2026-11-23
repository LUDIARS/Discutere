#include "flow/dialectic/drilldown.hpp"

#include <algorithm>

#include "base/strings.hpp"

namespace di::flow::dialectic {

namespace {

bool blank(const std::optional<std::string>& s) { return !s || str::trim(*s).empty(); }

const char* const kData = "反論のデータ (事実・事例・仕様。他のゲームでの例やプレイヤーの声でもよい)";
const char* const kWarrant = "反論の論拠 (そのデータで相手のどこが崩れるか)";

} // namespace

// 深掘りは論拠が不足している場合だけ (neco 2026-11-23)。突く先が無い (unanchored) のは論拠の不足ではないので対象外。
bool is_form_foul(const RuleFoul& foul) { return foul == "incomplete"; }

std::vector<std::string> missing_rebut_elements(const ParsedExchangeTurn& turn, const std::optional<RebutGateResult>& gate) {
    // 形が崩れて返ってきた応答は、データも論拠も読み取れない = 論拠が不足している。
    if (turn.degraded) return {kData, kWarrant};
    if (turn.act != "rebut") return {};
    if (gate && gate->accepted && !blank(turn.data)) return {};
    if (gate && !gate->accepted && !is_form_foul(gate->foul)) return {};
    std::vector<std::string> out;
    if (blank(turn.data)) out.push_back(kData);
    if (blank(turn.warrant)) out.push_back(kWarrant);
    return out;
}

std::string render_drilldown_request(const std::string& persona_name, const std::vector<std::string>& missing) {
    return persona_name + "さん、今の反論には「" + str::join(missing, "」「") +
           "」が足りないので、まだ根拠への反論として数えられません。足りないところを添えて、もう一度言い直してください。";
}

std::string drilldown_prompt_block(const std::vector<std::string>& missing, const std::string& previous_text) {
    std::vector<std::string> lines{"# 進行役からの深掘り (1 回だけの言い直し)", "あなたの前の応答: " + str::js_slice(str::squish(previous_text), 0, 200),
                                   "前の応答には次が足りなかった:"};
    for (const auto& m : missing) lines.push_back("- " + m);
    lines.emplace_back("論拠は次のどれかの形で立てる: 演繹 (この仕様を入れて、こういう人が遊ぶと、こういう体験になる) / "
                       "帰納 (この仕様を入れている他のゲームでは、こういう体験になっていた) / "
                       "アブダクション (こういう体験を作りたいから、きっとこういう仕様にしたら良い)。");
    lines.emplace_back("トゥールミンの形で立てる: 主張とデータを分け、データから主張へつなぐ論拠を 1 つ書く (データの言い換えにしない)。"
                       "論拠がなぜ成り立つかの裏付けがあれば添え、どの範囲・どの確度の話かをはっきりさせる。");
    lines.emplace_back("材料が無くて反論を立てられないなら、無理に作らず concede で認めるか、何の材料が足りないかを text に書く。");
    return str::join(lines, "\n");
}

std::vector<std::string> missing_position_elements(const GeneratedPosition& p, bool llm_error) {
    if (llm_error) return {};
    // 深掘りは論拠が不足している場合だけ (neco 2026-11-23)。種類・範囲・確度・裏付け・例外条件の欠けは
    // 指示文で求めるだけで深掘りしない。形が崩れた定立 (根拠が主張の複製) は論拠の不足として扱う。
    std::vector<std::string> out;
    const bool copied = std::any_of(p.grounds.begin(), p.grounds.end(), [&](const Ground& g) { return g.text == p.claim; });
    const bool no_warrant = std::any_of(p.grounds.begin(), p.grounds.end(), [](const Ground& g) { return blank(g.warrant); });
    const bool restated = std::any_of(p.grounds.begin(), p.grounds.end(), [](const Ground& g) {
        return !blank(g.warrant) && (str::trim(*g.warrant) == str::trim(g.text) || str::contains(g.text, str::trim(*g.warrant)));
    });
    if (p.degraded || copied) out.push_back("主張と別のデータ (事実・事例・仕様。主張をそのまま根拠にしない)");
    if (p.degraded || no_warrant) out.push_back("各根拠の論拠 (データから主張へどうつながるか)");
    if (!p.degraded && restated) out.push_back("データの言い換えではない論拠 (データからなぜ主張が言えるか)");
    return out;
}

std::string position_drilldown_feedback(const std::vector<std::string>& missing) {
    return "# 進行役からの深掘り (1 回だけの作り直し)\n前の定立には次が足りなかった:\n- " + str::join(missing, "\n- ") +
           "\n根拠は 演繹 / 帰納 / アブダクション のどれかの形で、トゥールミンの 6 要素 (主張・データ・論拠・裏付け・限定・反駁) で書く。"
           "データと論拠を分け、限定は範囲と確度を、反駁はこの主張が成り立たない場合 (例外条件) を書く。";
}

} // namespace di::flow::dialectic
