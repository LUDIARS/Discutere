#include "flow/spec_analyze.hpp"

#include <algorithm>
#include <set>

#include "base/strings.hpp"
#include "flow/mechanic_extract.hpp"

namespace di::flow {

namespace {

/** 1 チャンクあたりの最大文字数。 */
constexpr std::size_t SPEC_CHAR_CAP = 16000;
/** チャンク間のオーバーラップ (文脈の途切れを和らげる)。 */
constexpr std::size_t CHUNK_OVERLAP = 2000;
/** 処理するチャンクの最大数 (LLM コスト上限)。 */
constexpr std::size_t MAX_CHUNKS = 4;

std::optional<std::vector<std::string>> to_str_array(const Json& v) {
    if (!v.is_array()) return std::nullopt;
    std::vector<std::string> out;
    for (const auto& s : v)
        if (s.is_string() && !str::trim(s.get<std::string>()).empty()) out.push_back(str::trim(s.get<std::string>()));
    if (out.empty()) return std::nullopt;
    return out;
}

/** LLM 返答 1 件を GameMechanicEntry に正規化する (name 必須・型ガード)。 */
std::optional<GameMechanicEntry> normalize_entry(const Json& m) {
    if (!m.is_object()) return std::nullopt;
    auto get_str = [&](const char* key) -> std::optional<std::string> {
        auto it = m.find(key);
        if (it == m.end() || !it->is_string()) return std::nullopt;
        return it->get<std::string>();
    };
    const std::string name = str::trim(get_str("name").value_or(""));
    if (name.empty()) return std::nullopt;
    GameMechanicEntry e;
    e.name = name;
    if (auto d = get_str("description"); d && !str::trim(*d).empty()) e.description = str::trim(*d);
    if (auto a = get_str("intended_affect"); a && !str::trim(*a).empty()) e.intended_affect = str::trim(*a);
    if (auto v = get_str("intended_valence")) {
        const std::string lower = str::to_lower(str::trim(*v));
        if (lower == "positive" || lower == "negative" || lower == "neutral") e.intended_valence = lower;
    }
    if (m.contains("intended_aspects")) e.intended_aspects = to_str_array(m["intended_aspects"]);
    if (m.contains("intended_emotions")) e.intended_emotions = to_str_array(m["intended_emotions"]);
    return e;
}

/** name (lower) で重複除去し、target 件で切る。 */
std::vector<GameMechanicEntry> dedupe(const std::vector<GameMechanicEntry>& entries, int target) {
    std::set<std::string> seen;
    std::vector<GameMechanicEntry> out;
    for (const auto& e : entries) {
        if (!seen.insert(str::to_lower(str::trim(e.name))).second) continue;
        out.push_back(e);
        if (static_cast<long>(out.size()) >= target) break;
    }
    return out;
}

std::pair<std::string, std::string> build_prompt(const std::string& theme, const std::string& spec_text, int max) {
    std::string system =
        std::string("あなたはゲームデザイン分析者です。 渡されたゲーム仕様書から、 実装されている / 設計されている") +
        " **遊びのメカニクス** (プレイヤーの行動とその設計意図) を構造化して洗い出します。" +
        " 仕様書に書かれている内容に忠実に、 具体的なメカニクスを抽出してください。 返答は JSON 配列のみ。";
    std::string prompt = "# ゲーム / テーマ\n" + theme + "\n\n# 仕様書\n" + spec_text + "\n\n" +
                         "上記の仕様書から読み取れる遊びのメカニクスを最大 " + std::to_string(max) +
                         " 件、 次の JSON 配列だけで返してください" +
                         " (前後に説明やコードフェンスを付けない)。 各フィールドは仕様書から読み取れる範囲で埋め、" +
                         " 不明な任意フィールドは省略してください:\n" +
                         "[{\"name\":\"メカニクス名\",\"description\":\"プレイヤーの行動と設計意図 (1〜2文)\"," +
                         "\"intended_affect\":\"狙う感情 (任意)\",\"intended_valence\":\"positive|negative|neutral (任意)\"," +
                         "\"intended_aspects\":[\"設計面 (任意)\"],\"intended_emotions\":[\"想定感情 (任意)\"]}]";
    return {system, prompt};
}

std::vector<GameMechanicEntry> analyze_chunk(persona_engine::LLMClient& llm, const std::string& theme,
                                             const std::string& chunk, std::size_t chunk_index,
                                             const AnalyzeSpecMechanicsArgs& args, const WarnFn& warn) {
    const std::string label = "仕様書解析 (チャンク" + std::to_string(chunk_index + 1) + ")";
    persona_engine::LLMResult res;
    try {
        auto [system, prompt] = build_prompt(theme, chunk, args.maxMechanics);
        persona_engine::LLMInvokeArgs call;
        call.system = system;
        call.prompt = prompt;
        call.model = args.model;
        call.maxTokens = std::min(8000, std::max(2000, args.maxMechanics * 300));
        res = llm.invoke(call);
    } catch (const std::exception& e) {
        warn(label + " で例外: " + e.what());
        return {};
    }
    if (!res.ok) {
        warn(label + " に失敗: " + res.error);
        return {};
    }
    auto arr = extract_json_array(res.text);
    if (!arr) {
        warn(label + " の JSON 解析に失敗");
        return {};
    }
    std::vector<GameMechanicEntry> out;
    for (const auto& item : *arr)
        if (auto e = normalize_entry(item)) out.push_back(std::move(*e));
    return out;
}

} // namespace

std::vector<std::string> split_spec_into_chunks(const std::string& text) {
    const std::size_t length = str::js_length(text);
    if (length <= SPEC_CHAR_CAP) return {text};
    std::vector<std::string> chunks;
    const std::size_t step = SPEC_CHAR_CAP - CHUNK_OVERLAP;
    for (std::size_t i = 0; i < length && chunks.size() < MAX_CHUNKS; i += step)
        chunks.push_back(str::js_slice(text, static_cast<std::int64_t>(i), static_cast<std::int64_t>(i + SPEC_CHAR_CAP)));
    return chunks;
}

std::vector<GameMechanicEntry> analyze_spec_mechanics(persona_engine::LLMClient& llm, const AnalyzeSpecMechanicsArgs& args) {
    const WarnFn warn = args.warn ? args.warn : WarnFn([](const std::string&) {});
    const std::string spec_text = str::trim(args.specText);
    if (spec_text.empty()) return {};
    const auto chunks = split_spec_into_chunks(spec_text);
    if (chunks.size() > 1) {
        warn("仕様書が長いため " + std::to_string(chunks.size()) + " チャンクに分割して解析します (元 " +
             std::to_string(str::js_length(spec_text)) + " 文字、" + "最大 " + std::to_string(MAX_CHUNKS * SPEC_CHAR_CAP) +
             " 文字を対象)");
    }
    std::vector<GameMechanicEntry> all;
    for (std::size_t i = 0; i < chunks.size(); ++i) {
        auto entries = analyze_chunk(llm, args.theme, chunks[i], i, args, warn);
        all.insert(all.end(), entries.begin(), entries.end());
    }
    return dedupe(all, args.maxMechanics);
}

} // namespace di::flow
