#include "flow/effect_predict.hpp"

#include <algorithm>
#include <cmath>
#include <map>

#include "base/strings.hpp"
#include "flow/sentiment_vector.hpp"

namespace di::flow {

namespace {

const std::map<std::string, std::size_t>& dim_index() {
    static const std::map<std::string, std::size_t> index = [] {
        std::map<std::string, std::size_t> m;
        const auto& dims = vector_dims();
        for (std::size_t i = 0; i < dims.size(); ++i) m.emplace(dims[i], i);
        return m;
    }();
    return index;
}

double clamp(double v, double lo, double hi) { return std::max(lo, std::min(hi, v)); }

constexpr std::size_t kMechanicSample = 10;
constexpr int kMaxTokens = 1500;

std::optional<Json> parse_object(std::string_view text) {
    auto parsed = parse_json(text, llm_json_limits());
    if (parsed && parsed->is_object()) return parsed;
    return std::nullopt;
}

} // namespace

JsonLimits llm_json_limits() { return JsonLimits{4u << 20, 200'000, 64}; }

std::optional<Json> extract_json_object(std::string_view raw) {
    const auto start = raw.find('{');
    if (start == std::string_view::npos) return std::nullopt;
    const auto end = raw.rfind('}');
    if (end != std::string_view::npos && end > start) {
        if (auto obj = parse_object(raw.substr(start, end - start + 1))) return obj;
    }
    // 救済: 先頭の {...} をブレースマッチ (文字列内ブレースは無視) で切り出して個別パース。
    long depth = 0;
    bool in_str = false;
    bool esc = false;
    for (std::size_t i = start; i < raw.size(); ++i) {
        const char ch = raw[i];
        if (in_str) {
            if (esc) esc = false;
            else if (ch == '\\') esc = true;
            else if (ch == '"') in_str = false;
            continue;
        }
        if (ch == '"') in_str = true;
        else if (ch == '{') ++depth;
        else if (ch == '}') {
            --depth;
            if (depth == 0) return parse_object(raw.substr(start, i - start + 1));
        }
    }
    return std::nullopt;
}

Json to_json(const EffectChange& c) {
    Json o = Json::object();
    o["dim"] = c.dim;
    o["delta"] = c.delta;
    if (c.reason) o["reason"] = *c.reason;
    return o;
}

EffectChange effect_change_from_json(const Json& j) {
    EffectChange c;
    c.dim = json_string(j, "dim");
    c.delta = json_number(j, "delta");
    c.reason = json_opt_string(j, "reason");
    return c;
}

Json to_json(const EffectPrediction& p) {
    Json changes = Json::array();
    for (const auto& c : p.changes) changes.push_back(to_json(c));
    Json o = Json::object();
    o["changes"] = std::move(changes);
    o["confidence"] = p.confidence;
    return o;
}

std::optional<EffectPrediction> normalize_prediction(const Json& raw, const WarnFn& warn) {
    if (!raw.is_object()) return std::nullopt;
    auto it = raw.find("changes");
    if (it == raw.end() || !it->is_array()) return std::nullopt;
    EffectPrediction out;
    for (const auto& c : *it) {
        if (!c.is_object()) continue;
        const auto dim_it = c.find("dim");
        const std::string dim = (dim_it != c.end() && dim_it->is_string()) ? str::trim(dim_it->get<std::string>()) : "";
        const auto delta_it = c.find("delta");
        const bool has_delta = delta_it != c.end() && delta_it->is_number() && std::isfinite(delta_it->get<double>());
        if (dim.empty() || !has_delta) continue;
        if (!dim_index().count(dim)) {
            if (warn) warn("効果予測: 未知の次元名 \"" + dim + "\" を無視 (VECTOR_DIMS 外)");
            continue;
        }
        EffectChange change;
        change.dim = dim;
        change.delta = clamp(delta_it->get<double>(), -1, 1);
        const auto reason_it = c.find("reason");
        if (reason_it != c.end() && reason_it->is_string()) change.reason = reason_it->get<std::string>();
        out.changes.push_back(std::move(change));
    }
    const auto conf = raw.find("confidence");
    out.confidence = (conf != raw.end() && conf->is_string() && conf->get<std::string>() == "low") ? "low" : "high";
    return out;
}

std::vector<double> changes_to_vector(const std::vector<EffectChange>& changes) {
    std::vector<double> v(DIM, 0.0);
    for (const auto& c : changes) {
        auto it = dim_index().find(c.dim);
        if (it == dim_index().end()) continue;
        v[it->second] = clamp(c.delta, -1, 1);
    }
    return v;
}

namespace {

std::pair<std::string, std::string> build_prompt(const PredictEffectArgs& args) {
    std::string mechanics_text;
    if (args.mechanics.empty()) {
        mechanics_text = "(なし)";
    } else {
        std::vector<std::string> lines;
        for (std::size_t i = 0; i < args.mechanics.size() && i < kMechanicSample; ++i) {
            const auto& m = args.mechanics[i];
            lines.push_back("- " + m.name + ": " + str::js_slice(m.description, 0, 120));
        }
        mechanics_text = str::join(lines, "\n");
    }
    std::string system =
        std::string("あなたはゲームデザイン分析者です。改善議論で出た意見を **採用した場合に、プレイヤー体験が") +
        "どう変化するか** を予測します。意見の文面の感情ではなく、採用後の体験の変化を評価してください" +
        " (熱く前向きな文面でも、採用すると体験が悪化する提案は負の変化です)。返答は JSON オブジェクトのみ。";
    std::string prompt = "# テーマ\n" + args.theme + "\n\n# 既知のメカニクス (文脈)\n" + mechanics_text + "\n\n" +
                         "# 体験の次元 (この名前だけを使う)\n" + str::join(vector_dims(), ", ") + "\n\n" +
                         "# 意見\n" + args.opinion + "\n\n" +
                         "この意見を採用した場合に **変化しそうな次元だけ** を挙げ、変化量 delta (-1..1、負=悪化/正=改善) を" +
                         "予測してください。全次元を無理に挙げないこと。次の JSON オブジェクトだけで返してください" +
                         " (前後に説明やコードフェンスを付けない):\n" +
                         "{\"changes\":[{\"dim\":\"asp.fun\",\"delta\":0.4,\"reason\":\"変化の理由 (1文)\"}],\"confidence\":\"high または low\"}";
    return {system, prompt};
}

} // namespace

std::optional<EffectPrediction> predict_effect(persona_engine::LLMClient& llm, const PredictEffectArgs& args) {
    const WarnFn warn = args.warn ? args.warn : WarnFn([](const std::string&) {});
    persona_engine::LLMResult res;
    try {
        auto [system, prompt] = build_prompt(args);
        persona_engine::LLMInvokeArgs call;
        call.system = system;
        call.prompt = prompt;
        if (args.model && !args.model->empty()) call.model = *args.model;
        call.maxTokens = kMaxTokens;
        res = llm.invoke(call);
    } catch (const std::exception& e) {
        warn(std::string("効果予測で例外: ") + e.what());
        return std::nullopt;
    }
    if (!res.ok) {
        warn("効果予測に失敗: " + res.error);
        return std::nullopt;
    }
    auto obj = extract_json_object(res.text);
    if (!obj) {
        warn("効果予測の JSON 解析に失敗");
        return std::nullopt;
    }
    auto prediction = normalize_prediction(*obj, warn);
    if (!prediction) {
        warn("効果予測の changes が不正 (配列でない)");
        return std::nullopt;
    }
    return prediction;
}

} // namespace di::flow
