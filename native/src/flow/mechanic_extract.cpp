#include "flow/mechanic_extract.hpp"

#include <algorithm>
#include <set>

#include "base/strings.hpp"

namespace di::flow {

namespace {

constexpr std::size_t kVoiceSample = 25;

} // namespace

std::optional<Json> extract_json_array(std::string_view raw) {
    const auto start = raw.find('[');
    if (start == std::string_view::npos) return std::nullopt;
    // まず素直に全体パース (正常時はこれで通る)。
    const auto end = raw.rfind(']');
    if (end != std::string_view::npos && end > start) {
        auto parsed = parse_json(raw.substr(start, end - start + 1), llm_json_limits());
        if (parsed && parsed->is_array()) return parsed;
    }
    // 救済: 配列内の {...} を 1 個ずつブレースマッチで取り出し個別パースする
    // (文字列内ブレースは無視。途中で切れた末尾オブジェクトは自然に落ちる)。
    Json objs = Json::array();
    long depth = 0;
    std::optional<std::size_t> obj_start;
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
        else if (ch == '{') {
            if (depth == 0) obj_start = i;
            ++depth;
        } else if (ch == '}') {
            if (depth > 0) --depth;
            if (depth == 0 && obj_start) {
                if (auto parsed = parse_json(raw.substr(*obj_start, i - *obj_start + 1), llm_json_limits()))
                    objs.push_back(std::move(*parsed));
                obj_start.reset();
            }
        }
    }
    if (objs.empty()) return std::nullopt;
    return objs;
}

namespace {

std::optional<MechanicSummary> normalize_mechanic(const Json& raw) {
    if (!raw.is_object()) return std::nullopt;
    const auto name_it = raw.find("name");
    const std::string name = (name_it != raw.end() && name_it->is_string()) ? str::trim(name_it->get<std::string>()) : "";
    if (name.empty()) return std::nullopt;
    const auto affect_it = raw.find("intended_affect");
    const std::string affect =
        (affect_it != raw.end() && affect_it->is_string()) ? str::trim(affect_it->get<std::string>()) : "";
    const auto desc_it = raw.find("description");
    MechanicSummary m;
    m.name = name;
    m.description = (desc_it != raw.end() && desc_it->is_string()) ? str::trim(desc_it->get<std::string>()) : "";
    if (!affect.empty()) m.intended_affect = affect;
    // 出所ラベル (08): LLM 増補分は必ず source="llm" (仮説メカニクス節に分離される)。
    m.source = "llm";
    return m;
}

std::pair<std::string, std::string> build_prompt(const EnrichMechanicsArgs& args, int need) {
    std::string existing_text = "(なし)";
    if (!args.existing.empty()) {
        std::vector<std::string> lines;
        for (const auto& m : args.existing) lines.push_back("- " + m.name + ": " + m.description);
        existing_text = str::join(lines, "\n");
    }
    std::string voice_text = "(感想なし)";
    if (!args.voices.empty()) {
        std::vector<std::string> lines;
        for (std::size_t i = 0; i < args.voices.size() && i < kVoiceSample; ++i)
            lines.push_back(std::to_string(i + 1) + ". " + str::js_slice(args.voices[i].content, 0, 160));
        voice_text = str::join(lines, "\n");
    }
    std::string system =
        std::string("あなたはゲームデザイン分析者です。 議題のゲームについて、 プレイヤーの感想から **遊びのメカニクス**") +
        " (プレイヤーの行動とその設計意図) を洗い出します。 既出と重複しない具体的なメカニクスを、" +
        " 感想に根ざして抽出してください。 返答は JSON 配列のみ。";
    std::string prompt = "# 議題\n" + args.theme + "\n\n# 既出メカニクス (重複させない)\n" + existing_text + "\n\n" +
                         "# プレイヤーの感想 (抽出の根拠)\n" + voice_text + "\n\n" +
                         "上記の感想から読み取れる **追加の** 遊びのメカニクスを最大 " + std::to_string(need) +
                         " 件、 次の JSON 配列だけで返してください" + " (前後に説明やコードフェンスを付けない):\n" +
                         "[{\"name\":\"メカニクス名\",\"description\":\"プレイヤーの行動と設計意図 (1〜2文)\",\"intended_affect\":\"狙う感情 (任意)\"}]";
    return {system, prompt};
}

/** JS の arr.slice(0, end) (負数は末尾から)。 */
std::vector<MechanicSummary> slice_to(const std::vector<MechanicSummary>& v, long end) {
    const long n = static_cast<long>(v.size());
    long e = end < 0 ? std::max(0L, n + end) : std::min(n, end);
    return std::vector<MechanicSummary>(v.begin(), v.begin() + e);
}

std::string name_key(const std::string& name) { return str::to_lower(str::trim(name)); }

/** name (lower) で重複除去しつつ既存 → 追加の順に結合し、target 件で切る。 */
std::vector<MechanicSummary> merge_mechanics(const std::vector<MechanicSummary>& existing,
                                             const std::vector<MechanicSummary>& added, int target) {
    std::set<std::string> seen;
    for (const auto& m : existing) seen.insert(name_key(m.name));
    std::vector<MechanicSummary> out = existing;
    for (const auto& m : added) {
        if (!seen.insert(name_key(m.name)).second) continue;
        out.push_back(m);
        if (static_cast<long>(out.size()) >= target) break;
    }
    return slice_to(out, target);
}

} // namespace

std::vector<MechanicSummary> enrich_mechanics(persona_engine::LLMClient& llm, const EnrichMechanicsArgs& args) {
    const WarnFn warn = args.warn ? args.warn : WarnFn([](const std::string&) {});
    const long need = static_cast<long>(args.target) - static_cast<long>(args.existing.size());
    if (need <= 0) return slice_to(args.existing, args.target);
    persona_engine::LLMResult res;
    try {
        auto [system, prompt] = build_prompt(args, static_cast<int>(need));
        persona_engine::LLMInvokeArgs call;
        call.system = system;
        call.prompt = prompt;
        call.model = args.model;
        // need 件の詳細メカニクスを途中で切らさないよう余裕を持たせる (~250 tokens/件 目安 + 下限)。
        call.maxTokens = static_cast<int>(std::min<long>(8000, std::max<long>(2000, need * 280)));
        res = llm.invoke(call);
    } catch (const std::exception& e) {
        warn(std::string("メカニクス増補で例外: ") + e.what());
        return args.existing;
    }
    if (!res.ok) {
        warn("メカニクス増補に失敗: " + res.error);
        return args.existing;
    }
    auto arr = extract_json_array(res.text);
    if (!arr) {
        warn("メカニクス増補の JSON 解析に失敗");
        return args.existing;
    }
    std::vector<MechanicSummary> added;
    for (const auto& item : *arr)
        if (auto m = normalize_mechanic(item)) added.push_back(std::move(*m));
    return merge_mechanics(args.existing, added, args.target);
}

} // namespace di::flow
