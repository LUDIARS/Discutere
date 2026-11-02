#include "flow/anatomia/refine.hpp"

#include "base/json.hpp"
#include "base/strings.hpp"

namespace di::flow::anatomia {

std::vector<MechanicSummary> fallback_domains_to_mechanics(const std::vector<AnatomiaDomainCard>& cards) {
    std::vector<MechanicSummary> out;
    for (const auto& c : cards) {
        if (c.mechanics.empty()) continue;
        const std::string slugs = str::join(c.mechanics, ", ");
        MechanicSummary m;
        m.name = c.name;
        m.description = !slugs.empty() ? c.description + " (実装メカニクス: " + slugs + ")" : c.description;
        out.push_back(std::move(m));
    }
    return out;
}

std::optional<std::vector<MechanicSummary>> parse_refine_response(const std::string& raw) {
    const auto start = raw.find('[');
    const auto end = raw.rfind(']');
    if (start == std::string::npos || end == std::string::npos || end <= start) return std::nullopt;
    auto parsed = parse_json(std::string_view(raw).substr(start, end - start + 1), JsonLimits{kRefineResponseMaxBytes, 100'000, 32});
    if (!parsed || !parsed->is_array()) return std::nullopt;
    std::vector<MechanicSummary> out;
    for (const auto& item : *parsed) {
        if (!item.is_object()) continue;
        const std::string name = item.contains("name") && item["name"].is_string() ? str::trim(item["name"].get<std::string>()) : "";
        if (name.empty()) continue;
        MechanicSummary m;
        m.name = name;
        m.description = item.contains("description") && item["description"].is_string() ? str::trim(item["description"].get<std::string>()) : "";
        const std::string affect =
            item.contains("intended_affect") && item["intended_affect"].is_string() ? str::trim(item["intended_affect"].get<std::string>()) : "";
        if (!affect.empty()) m.intended_affect = affect;
        out.push_back(std::move(m));
    }
    return out;
}

std::string build_refine_prompt(const std::vector<AnatomiaDomainCard>& cards, const std::optional<std::string>& theme) {
    std::vector<std::string> lines;
    for (const auto& c : cards) {
        const std::string slugs = !c.mechanics.empty() ? " / 実装メカニクス: " + str::join(c.mechanics, ", ") : "";
        lines.push_back("- " + c.name + ": " + c.description + slugs);
    }
    return std::string("あなたはゲームデザインのアナリストです。以下は Anatomia がソースコード解析から抽出した") +
           "あるゲームの「ドメイン (実装上の責務の塊)」一覧です。\n\n" + str::join(lines, "\n") + "\n\n" +
           (theme && !theme->empty() ? "議論テーマ: " + *theme + "\n\n" : "") +
           "この一覧から、**プレイヤーが体験する「遊びのメカニクス」** だけを抽出してください。\n" +
           "- インフラ層 (LLM backend, サーバ/通信プロトコル, データ配置)・表示層 (UI/レンダリング)・" +
           "内部基盤は **除外** する。\n" + "- 採用する各メカニクスは、description を **プレイヤー視点** で言い直し、" +
           "intended_affect に「そのメカニクスが狙う体験/感情」を 1 文で書く。\n" + "- 実装用語ではなく、遊びとして何が起きるかを書く。\n\n" +
           "出力は JSON 配列のみ (前置き/コードフェンス無し):\n" + "[{\"name\":string,\"description\":string,\"intended_affect\":string}]";
}

std::vector<MechanicSummary> refine_domains_to_mechanics(const std::vector<AnatomiaDomainCard>& cards, const RefineDomainsDeps& deps) {
    const auto warn = [&](const std::string& m) {
        if (deps.warn) deps.warn(m);
    };
    if (cards.empty()) return {};
    if (!deps.llm) return fallback_domains_to_mechanics(cards);
    persona_engine::LLMInvokeArgs args;
    args.prompt = build_refine_prompt(cards, deps.theme);
    if (deps.model && !deps.model->empty()) args.model = deps.model;
    const auto result = deps.llm->invoke(args);
    if (!result.ok) {
        warn("Anatomia 精製 LLM 失敗 (" + result.error + ") → 決定論フォールバック");
        return fallback_domains_to_mechanics(cards);
    }
    auto mechanics = parse_refine_response(result.text);
    if (!mechanics || mechanics->empty()) {
        warn("Anatomia 精製 LLM 応答が空/不正 → 決定論フォールバック");
        return fallback_domains_to_mechanics(cards);
    }
    return *mechanics;
}

} // namespace di::flow::anatomia
