#include "flow/dialectic/premise_rules.hpp"

#include "base/strings.hpp"

namespace di::flow::dialectic {

namespace {
struct Pair {
    const char* key;
    const char* label;
};
constexpr Pair kInference[] = {{"deduction", "演繹"}, {"induction", "帰納"}, {"abduction", "アブダクション"}};
constexpr Pair kAttack[] = {{"data", "データ"}, {"warrant", "論拠"}, {"qualifier", "限定"}};
} // namespace

std::string inference_kind_label(const InferenceKind& kind) {
    for (const auto& p : kInference)
        if (kind == p.key) return p.label;
    return "";
}

std::string attack_point_label(const AttackPoint& point) {
    for (const auto& p : kAttack)
        if (point == p.key) return p.label;
    return "";
}

std::optional<InferenceKind> coerce_inference_kind(const Json& v) {
    if (!v.is_string()) return std::nullopt;
    const std::string s = str::to_lower(str::trim(v.get<std::string>()));
    for (const auto& p : kInference)
        if (s == p.key) return std::string(p.key);
    for (const auto& p : kInference)
        if (s == str::to_lower(p.label)) return std::string(p.key);
    return std::nullopt;
}

std::optional<AttackPoint> coerce_attack_point(const Json& v) {
    if (!v.is_string()) return std::nullopt;
    const std::string s = str::to_lower(str::trim(v.get<std::string>()));
    for (const auto& p : kAttack)
        if (s == p.key) return std::string(p.key);
    for (const auto& p : kAttack)
        if (s == p.label) return std::string(p.key);
    return std::nullopt;
}

const std::string& premise_rules_text() {
    static const std::string text = str::join(
        {
            "# 議論の前提ルール (厳守)",
            "次の 3 点について共通見解は取れている前提で議論する (すり合わせ直さない):",
            "- どんな体験をつくりたいか",
            "- どんな施策 (アイデア) を試したいか",
            "- 他のゲームではどうしているか、どうなっているか",
            "論拠をぶつけ合った結果、論点の結論は次のいずれかになる: 止揚 (両者の根拠を保ったまま一段上の結論に至る) / 論破 (一方の根拠がすべて崩れ、意見が出なくなる) / 結論なし (どちらにも至らない)。",
            "",
            "論拠は次の 3 種類のどれかで立てる:",
            "- 演繹 (deduction): この仕様を入れて、こういう人が遊ぶと、こういう体験になる",
            "- 帰納 (induction): この仕様を入れている他のゲームでは、こういう体験になっていた (同じ仕様であることを示す)",
            "- アブダクション (abduction): こういう体験を作りたいから、きっとこういう仕様にしたら良い",
            "",
            "トゥールミンの整理:",
            "- 主張とデータは別物。データ (事実・事例) と論拠 (データから主張へ橋渡しする理由) を混ぜない",
            "- データが正しくても論拠が間違っていれば主張は間違いになる。反論はデータ・論拠・限定のどれを突くか決めて行う",
            "- 主張の限定 (どのプレイヤー層・どの条件の話か) をはっきりさせる",
            "",
            "禁止 (前提の外から相手の主張を曲げる手):",
            "- 相手の限定の外側にある層・条件・事例を持ち出して主張全体を否定する",
            "- 相手が言っていない主張に置き換えて叩く",
            "- 根拠の中身に触れず「それって感想」「データあるの?」だけで退ける (足りないなら何が要るかを具体的に問う)",
            "- 論点をずらす、別の論点にすり替える",
            "- 何を論拠として認めるかという前提を途中で変える",
        },
        "\n");
    return text;
}

} // namespace di::flow::dialectic
