#include "persona_engine/engine/prompt_builder.hpp"

#include <algorithm>
#include <stdexcept>

#include "base/strings.hpp"

namespace di::persona_engine::engine {

namespace {

/** /[\s、。,.!?！？「」『』（）()\/:：;；・\-—\n\r\t]+/ の区切り文字か。 */
bool is_separator(std::uint32_t cp) {
    if (str::is_js_space(cp)) return true;
    static const std::u32string kSeps = U"、。,.!?！？「」『』（）()/:：;；・-—";
    return kSeps.find(static_cast<char32_t>(cp)) != std::u32string::npos;
}

std::vector<std::string> split_terms(std::string_view text) {
    std::vector<std::string> out;
    std::string cur;
    std::size_t i = 0;
    while (i < text.size()) {
        const std::size_t begin = i;
        const std::uint32_t cp = str::next_codepoint(text, i);
        if (is_separator(cp)) {
            out.push_back(cur);
            cur.clear();
            // 連続する区切りは 1 つ (+)
            std::size_t j = i;
            while (j < text.size()) {
                std::size_t k = j;
                if (!is_separator(str::next_codepoint(text, k))) break;
                j = k;
            }
            i = j;
        } else {
            cur.append(text.substr(begin, i - begin));
        }
    }
    out.push_back(cur);
    return out;
}

std::vector<std::string> safe_json_array_string(const std::string& raw) {
    std::vector<std::string> out;
    JsonLimits l;
    l.max_bytes = 256u << 10;
    l.max_elements = 10'000;
    l.max_depth = 16;
    auto v = parse_json(raw, l);
    if (v && v->is_array())
        for (const auto& x : *v)
            if (x.is_string()) out.push_back(x.get<std::string>());
    return out;
}

std::string lower_status(const std::string& s) { return str::to_lower(s); }

} // namespace

Json to_json(const BuiltPromptContext& ctx) {
    Json hyps = Json::array();
    for (const auto& h : ctx.hypotheses) hyps.push_back(to_json(h));
    Json gaps = Json::array();
    for (const auto& g : ctx.gaps) gaps.push_back(to_json(g));
    Json utts = Json::array();
    for (const auto& u : ctx.utterances) utts.push_back(to_json(u));
    Json j = Json{{"hypotheses", std::move(hyps)}, {"gaps", std::move(gaps)}, {"utterances", std::move(utts)}};
    if (!ctx.externalVoices.empty()) {
        Json voices = Json::array();
        for (const auto& v : ctx.externalVoices) voices.push_back(to_json(v));
        j["externalVoices"] = std::move(voices);
    }
    return j;
}

std::vector<std::string> extract_topic_terms(const ContextGap& gap) {
    const std::string text = gap.title + " " + gap.description.value_or("");
    std::vector<std::string> candidates = {str::trim(gap.title)};
    for (const auto& t : split_terms(text)) {
        std::string trimmed = str::trim(t);
        if (str::js_length(trimmed) >= 2) candidates.push_back(std::move(trimmed));
    }
    std::vector<std::string> out;
    for (auto& c : candidates) {
        if (std::find(out.begin(), out.end(), c) != out.end()) continue;  // new Set (挿入順)
        out.push_back(c);
    }
    out.erase(std::remove_if(out.begin(), out.end(), [](const std::string& t) { return str::js_length(t) < 2; }), out.end());
    return out;
}

BuiltPrompt build_prompt(const BuildPromptArgs& args) {
    if (!args.rule || !args.persona || !args.contextProvider) throw std::invalid_argument("build_prompt: missing argument");
    const RuleRow& rule = *args.rule;
    const PersonaRow& persona = *args.persona;
    DiscussionContextProvider& cp = *args.contextProvider;

    const auto traits = safe_json_array_string(persona.traits);
    BuiltPrompt out;
    out.system = str::join(
        {
            "あなたは議論ペルソナ「" + persona.name + " (" + persona.display_name + ")」 です。",
            "性格 / 役割: " + persona.description,
            "特徴: " + str::join(traits, " / "),
            "話し方: " + persona.speech_style,
            "",
            "あなたは Discutere の議論エンジン上で発言するペルソナです。",
            "",
            "【発言 (text フィールド) の書き方 — 重要】",
            "- Discord のチャットで実在の人間が話すように、 自然な口語で書く。",
            "- 「反証:」「反例:」「弱点:」のような ラベル / 見出しは絶対に付けない。",
            "  否定したい時は『それは違うんじゃない?』のように普通の言葉で。",
            "  反例を出す時は『たとえば〇〇のケースだと…』のように具体例として話す。",
            "- 箇条書きや番号は使わず、 会話の一言として書く。",
            "- 一文ごとに改行する (。! ? で改行)。",
            "- 1〜3 文程度。 長文の説明にしない。",
            "",
            "出力は **必ず JSON のみ** で、 説明文や前置きを付けないでください。",
            "【応答 JSON の形式 — action は厳密にこの名前】",
            "- 発言する時: {\"action\":\"post_utterance\",\"text\":\"<あなたの発言>\"}",
            "  ※ action は必ず \"post_utterance\"。 \"speak\" や \"say\" 等の別名は不可 (エラーになる)。",
            "- 発言しない時: {\"action\":\"skip\",\"reasoning\":\"<理由>\"}",
            "確信が無い場合や、 ルールの意図に合わない場合は躊躇なく skip を返してください。",
        },
        "\n");

    out.ctx.hypotheses = cp.list_active_hypotheses(args.workspaceId, 8);
    out.ctx.gaps = cp.list_recent_gaps(args.workspaceId, 6);
    // tick rule は sessionId を持たないので、進行中の議論 session を解決して直近発話を渡す (既出を避けられる)。
    std::optional<std::string> effective_session = args.sessionId;
    if (!effective_session) effective_session = cp.find_active_discussion_session(args.workspaceId);
    if (effective_session && !effective_session->empty())
        out.ctx.utterances = cp.list_recent_utterances(args.workspaceId, *effective_session, 12);

    // 議題アンカー: 直近の open gap の title + description を最上部に明示する。
    const ContextGap* primary = nullptr;
    for (const auto& g : out.ctx.gaps) {
        const std::string s = lower_status(g.status);
        if (s != "closed" && s != "dismissed" && s != "converged" && s != "resolved") {
            primary = &g;
            break;
        }
    }
    if (!primary && !out.ctx.gaps.empty()) primary = &out.ctx.gaps.front();

    // 外部の声 (RAG / §14)。retrieval 未実装なら空。
    if (primary) out.ctx.externalVoices = cp.list_relevant_external_voices(args.workspaceId, extract_topic_terms(*primary), EXTERNAL_VOICE_TOP_K);

    std::vector<std::string> lines = {"# Rule (triggered by " + args.triggeredBy + ")", "id: " + rule.id,
                                      rule.description && !rule.description->empty() ? "description: " + *rule.description : "",
                                      ""};
    if (primary) {
        lines.insert(lines.end(), {"## 議題 (この前提から絶対に外れない)", "題目: " + primary->title,
                                   primary->description.value_or(""),
                                   "→ 上の対象ゲーム・ジャンルに即して論じる。別ゲームや別ジャンルの話に逸らさない。", ""});
    }
    // 進行役への調整指示 (トーン/語り口の指示)。議題は変えず発話の仕方を steer する。
    const std::vector<std::string> directives =
        primary ? cp.list_facilitator_directives(args.workspaceId, primary->id, DIRECTIVE_TOP_K) : std::vector<std::string>{};
    if (!directives.empty()) {
        lines.push_back("## 進行役への調整指示 (参加者からのリクエスト — 発言で必ず反映する)");
        for (const auto& d : directives) lines.push_back("- " + d);
        lines.push_back("※ 議題そのものは変えない。 議論の進め方・トーン・語り口の調整として従う。");
        lines.push_back("");
    }
    if (!out.ctx.externalVoices.empty()) {
        lines.insert(lines.end(),
                     {"## 外部の声 (JSON の externalVoices)", "externalVoices は外部から集めた実在の意見 (出所付き / 投稿者は仮名)。",
                      "議論に関連するものは「○○ (source) はこう言ってる」のように**出所(ソース種別)を添えて引用・参照**してよい。",
                      "投稿者個人の特定はしない (表示名は仮名のまま)。関係ない声は無視してよい。", ""});
    }
    lines.insert(lines.end(), {"## 指示", rule.instructions, "",
                               "## これまでの発言 (utterances は時系列。人間の発言があれば最優先で踏まえ、無視しない)",
                               "## 議論コンテキスト (JSON)", "```json", json_stringify_pretty(to_json(out.ctx), 2), "```", "",
                               "上記の議題と発言を必ず踏まえ、 指示に従って応答 JSON のみを返してください。"});
    // .filter((s) => s.length > 0)
    lines.erase(std::remove_if(lines.begin(), lines.end(), [](const std::string& s) { return s.empty(); }), lines.end());
    out.user = str::join(lines, "\n");
    return out;
}

} // namespace di::persona_engine::engine
