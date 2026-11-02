#include "flow/debatability.hpp"

#include <algorithm>
#include <cmath>
#include <map>

#include "base/strings.hpp"
#include "flow/mechanic_extract.hpp"
#include "flow/sentiment_vector.hpp"
#include "flow/user_voices/reference_policy.hpp"
#include "lib/sentiment_core/js_number.hpp"

namespace di::flow {

namespace {

using lib::sentiment_core::js_round_fixed;
using lib::sentiment_core::js_to_fixed;
using lib::sentiment_core::js_to_number;

constexpr std::size_t EVIDENCE_LIMIT = 50;
constexpr double SKEW_THRESHOLD = 0.7;
constexpr double VALENCE_NEUTRAL = 0.05;
constexpr std::size_t ARMABILITY_VOICE_SAMPLE = 20;
constexpr std::int64_t VOICE_TRIM = 160;

bool is_armable(const Json& v) {
    if (!v.is_string()) return false;
    const auto& s = v.get_ref<const std::string&>();
    return s == "both" || s == "pro-only" || s == "con-only" || s == "neither";
}

} // namespace

Json to_json(const EvidenceBalance& e) {
    Json o = Json::object();
    o["voiceCount"] = e.voiceCount;
    o["positive"] = e.positive;
    o["negative"] = e.negative;
    o["neutral"] = e.neutral;
    o["polaritySkew"] = e.polaritySkew;
    if (e.dominantSource) {
        Json d = Json::object();
        d["name"] = e.dominantSource->name;
        d["share"] = e.dominantSource->share;
        o["dominantSource"] = std::move(d);
    } else {
        o["dominantSource"] = nullptr;
    }
    return o;
}

Json to_json(const DebatabilityResult& r) {
    Json o = Json::object();
    o["issues"] = r.issues;
    Json arm = Json::array();
    for (const auto& a : r.armability) arm.push_back(Json{{"issue", a.issue}, {"armable", a.armable}});
    o["armability"] = std::move(arm);
    o["armableBothCount"] = r.armableBothCount;
    o["minArmableIssues"] = r.minArmableIssues;
    o["evidence"] = to_json(r.evidence);
    o["debatable"] = r.debatable;
    o["degraded"] = r.degraded;
    o["recommendation"] =
        r.recommendation ? Json{{"flow", r.recommendation->flow}, {"reason", r.recommendation->reason}} : Json(nullptr);
    o["message"] = r.message;
    return o;
}

EvidenceBalance compute_evidence_balance(const std::vector<DebatabilityVoice>& voices, const ToVectorFn& to_vector) {
    const std::size_t n = std::min(voices.size(), EVIDENCE_LIMIT);
    int positive = 0, negative = 0, neutral = 0;
    std::vector<std::pair<std::string, int>> by_source;  // Map の挿入順
    for (std::size_t i = 0; i < n; ++i) {
        const auto& v = voices[i];
        const auto vec = to_vector ? to_vector(v.content) : text_to_vector(v.content);
        const double valence = (vec.empty() ? std::nan("") : vec[0]) * 2 - 1;
        if (valence > VALENCE_NEUTRAL) ++positive;
        else if (valence < -VALENCE_NEUTRAL) ++negative;
        else ++neutral;
        std::string src = str::trim(v.source.value_or("unknown"));
        if (src.empty()) src = "unknown";
        auto it = std::find_if(by_source.begin(), by_source.end(), [&](const auto& kv) { return kv.first == src; });
        if (it == by_source.end()) by_source.emplace_back(src, 1);
        else ++it->second;
    }
    const int polarized = positive + negative;
    const double skew = polarized > 0 ? static_cast<double>(positive - negative) / polarized : 0;
    EvidenceBalance e;
    e.voiceCount = static_cast<double>(n);
    e.positive = positive;
    e.negative = negative;
    e.neutral = neutral;
    e.polaritySkew = js_round_fixed(skew, 4);
    std::optional<DominantSource> dominant;
    for (const auto& [name, count] : by_source) {
        const double share = static_cast<double>(count) / static_cast<double>(n);
        if (!dominant || share > dominant->share) dominant = DominantSource{name, share};
    }
    if (dominant) e.dominantSource = DominantSource{dominant->name, js_round_fixed(dominant->share, 4)};
    return e;
}

std::optional<std::vector<IssueArmability>> judge_issue_armability(persona_engine::LLMClient& llm,
                                                                    const JudgeArmabilityArgs& args) {
    const WarnFn warn = args.warn ? args.warn : WarnFn([](const std::string&) {});
    if (args.issues.empty()) return std::vector<IssueArmability>{};
    std::vector<std::string> issue_lines;
    for (std::size_t i = 0; i < args.issues.size(); ++i) issue_lines.push_back(std::to_string(i + 1) + ". " + args.issues[i]);
    std::string voice_text = "(材料なし)";
    if (!args.voices.empty()) {
        std::vector<std::string> lines;
        for (std::size_t i = 0; i < args.voices.size() && i < ARMABILITY_VOICE_SAMPLE; ++i)
            lines.push_back(std::to_string(i + 1) + ". " + str::js_slice(args.voices[i].content, 0, VOICE_TRIM));
        voice_text = str::join(lines, "\n");
    }
    persona_engine::LLMInvokeArgs call;
    call.system = std::string(user_voices::VOICE_REFERENCE_POLICY) +
                  "\nあなたは議論の準備担当です。 各論点について、手元の材料と前提・事前知識で" +
                  " 賛成側 (pro) と反対側 (con) の両方が根拠を張れるかを判定します。 返答は JSON 配列のみ。";
    call.prompt = "# 議題\n" + args.theme + "\n\n# 論点\n" + str::join(issue_lines, "\n") + "\n\n# 手元の材料 (外部の声)\n" +
                  voice_text + "\n\n" + "各論点について、この材料で pro / con のどちら側が根拠を持てるかを判定し、" +
                  "次の JSON 配列だけを返してください (index は論点番号 1 始まり):\n" +
                  "[{\"index\":1,\"armable\":\"both\"|\"pro-only\"|\"con-only\"|\"neither\"}]";
    call.model = args.model;
    persona_engine::LLMResult res;
    try {
        res = llm.invoke(call);
    } catch (const std::exception& e) {
        warn(std::string("両論武装判定で例外: ") + e.what());
        return std::nullopt;
    }
    if (!res.ok) {
        warn("両論武装判定に失敗: " + res.error);
        return std::nullopt;
    }
    auto arr = extract_json_array(res.text);
    if (!arr) {
        warn("両論武装判定の JSON 解析に失敗");
        return std::nullopt;
    }
    std::map<double, IssueArmable> by_index;
    for (const auto& raw : *arr) {
        if (!raw.is_object()) continue;
        const Json idx_json = raw.contains("index") ? raw["index"] : Json();
        // typeof index === "number" ? index : Number(index) (undefined は NaN)
        const double idx = raw.contains("index") ? js_to_number(idx_json) : std::nan("");
        const Json armable = raw.contains("armable") ? raw["armable"] : Json();
        if (!std::isfinite(idx) || !is_armable(armable)) continue;
        double t = std::trunc(idx);
        if (t == 0) t = 0;  // -0 → 0
        by_index[t] = armable.get<std::string>();
    }
    if (by_index.empty()) {
        warn("両論武装判定の結果が空 (解析不能)");
        return std::nullopt;
    }
    // 判定が欠けた issue は保守的に neither とする。
    std::vector<IssueArmability> out;
    for (std::size_t i = 0; i < args.issues.size(); ++i) {
        auto it = by_index.find(static_cast<double>(i + 1));
        out.push_back({args.issues[i], it == by_index.end() ? "neither" : it->second});
    }
    return out;
}

DebatabilityResult assess_debatability(persona_engine::LLMClient& llm, const AssessDebatabilityArgs& args) {
    const WarnFn warn = args.warn ? args.warn : WarnFn([](const std::string&) {});
    // [3] 証拠バランス (機械計算・LLM 不使用) — LLM 失敗時も必ず得られる。
    const auto evidence = compute_evidence_balance(args.voices, args.toVector);
    auto degraded_result = [&](const std::string& why) {
        DebatabilityResult r;
        r.minArmableIssues = args.minArmableIssues;
        r.evidence = evidence;
        r.debatable = true;
        r.degraded = true;
        r.message = "議論適性ゲートを実行できませんでした (" + why + ")。適性あり扱いで続行します。";
        return r;
    };

    // [1] 争点の存在。
    std::vector<std::string> issues;
    try {
        dialectic::DecomposeIssuesArgs d;
        d.theme = args.theme;
        d.paperMd = args.paperMd;
        d.model = args.model;
        d.maxIssues = dialectic::ISSUES_MAX;
        d.warn = warn;
        issues = args.decompose ? args.decompose(llm, d) : dialectic::decompose_issues(llm, d);
    } catch (const std::exception& e) {
        warn(std::string("議論適性ゲート degrade: ") + e.what());
        return degraded_result("論点分解に失敗");
    }

    // [2] 両論武装可能性。
    const auto armability = judge_issue_armability(llm, {args.theme, issues, args.voices, args.model, warn});
    if (!armability) {
        warn("議論適性ゲート degrade: 両論武装判定に失敗");
        auto r = degraded_result("両論武装判定に失敗");
        r.issues = issues;
        return r;
    }
    int both = 0;
    for (const auto& a : *armability)
        if (a.armable == "both") ++both;
    DebatabilityResult r;
    r.issues = issues;
    r.armability = *armability;
    r.armableBothCount = both;
    r.minArmableIssues = args.minArmableIssues;
    r.evidence = evidence;
    // 声の量・偏り・両論の根拠不足は参考情報。開始条件にしない。論点はペーパーで修正する (再提案はしない)。
    r.debatable = !issues.empty();
    r.degraded = false;
    const std::string skew_note =
        std::abs(evidence.polaritySkew) >= SKEW_THRESHOLD ? " / 極性偏り " + js_to_fixed(evidence.polaritySkew, 2) : "";
    const std::string counts = std::to_string(both) + "/" + std::to_string(issues.size()) + " 件";
    r.message = r.debatable ? "議論可能（外部の声は任意の参考値） (両論の参考根拠がある争点 " + counts + skew_note + ")"
                            : "議論適性: 低 (両論武装可能な争点 " + counts + " < " + std::to_string(args.minArmableIssues) +
                                  skew_note + ")";
    return r;
}

std::string armable_annotation(const IssueArmable& armable) {
    if (armable == "both") return "(両論: ○)";
    if (armable == "pro-only") return "(両論: 賛成側のみ)";
    if (armable == "con-only") return "(両論: 反対側のみ)";
    if (armable == "neither") return "(両論: ×)";
    return "";
}

std::vector<std::string> annotated_issues(const DebatabilityResult& result) {
    std::map<std::string, IssueArmable> by_issue;
    for (const auto& a : result.armability) by_issue[a.issue] = a.armable;
    std::vector<std::string> out;
    for (const auto& issue : result.issues) {
        auto it = by_issue.find(issue);
        out.push_back(it == by_issue.end() ? issue : issue + " " + armable_annotation(it->second));
    }
    return out;
}

} // namespace di::flow
