#include "flow/proxy/bundle.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include "base/strings.hpp"

namespace di::flow::proxy {
namespace {

const std::map<std::string, std::string>& dim_labels() {
    static const std::map<std::string, std::string> labels = {
        {"emo.valence", "全体の好意"},
        {"emo.arousal", "熱量"},
        {"emo.joy", "楽しさ・喜び"},
        {"emo.trust", "信頼"},
        {"emo.fear", "不安"},
        {"emo.surprise", "驚き"},
        {"emo.sadness", "寂しさ"},
        {"emo.disgust", "嫌悪"},
        {"emo.anger", "怒り"},
        {"emo.anticipation", "期待"},
        {"asp.fun", "遊びの面白さ"},
        {"asp.difficulty", "難易度"},
        {"asp.content", "遊べる量"},
        {"asp.price_value", "値段に見合うか"},
        {"asp.performance", "動作の快適さ"},
        {"asp.story", "物語"},
        {"asp.graphics", "見た目"},
        {"asp.replayability", "繰り返し遊べるか"},
    };
    return labels;
}

int relevance(const PackEvidence& e, const std::vector<std::string>& terms) {
    int score = 0;
    for (const auto& t : terms)
        if (!t.empty() && (str::contains(e.text, t) || str::contains(e.gameTitle, t))) ++score;
    return score;
}

std::vector<std::string> map_labels(const std::vector<std::string>& dims) {
    std::vector<std::string> out;
    for (const auto& d : dims) out.push_back(dim_label(d));
    return out;
}

std::vector<std::string> header(const PersonaPack& pack, const std::vector<ConditionalGap>& gaps, const std::string& label) {
    std::vector<std::string> lines = {
        "# あなたが代理する人物: " + label,
        "この人物は公開レビューの発言から作った AI の代理です。本人ではなく、本人の発言・同意でもありません。",
        "ジャンル: " + pack.genre + " / 材料: 発言 " + js_number_to_string(pack.sufficiency.utterances) + " 件 (好意的 " +
            js_number_to_string(pack.sufficiency.positive) + "・否定的 " + js_number_to_string(pack.sufficiency.negative) +
            ")、ゲーム " + js_number_to_string(pack.sufficiency.games) + " 本",
    };
    const auto wants = map_labels(pack.designGap.wants);
    const auto complaints = map_labels(pack.designGap.complaints);
    if (!wants.empty()) lines.push_back("このジャンルで求めているもの: " + str::join(wants, "・"));
    if (!complaints.empty()) lines.push_back("このジャンルで不満を持ちやすいもの: " + str::join(complaints, "・"));
    for (const auto& g : gaps) {
        lines.push_back("望み: " + g.current + " → " + g.desired +
                        (g.condition && !g.condition->empty() ? " (条件: " + *g.condition + ")" : std::string()) + " [" +
                        str::join(g.evidenceIds, ",") + "]");
    }
    if (!pack.styleNotes.empty()) lines.push_back("話し方の癖: " + str::join(pack.styleNotes, " / "));
    return lines;
}

std::string card_line(const StanceCard& c) {
    return "- [" + c.id + "] " + (c.polarity == "pos" ? "肯定" : "否定") + ": " + c.claim + " (根拠 " + str::join(c.evidenceIds, ",") + ")";
}

std::string evidence_line(const PackEvidence& e) {
    return "- [" + e.id + "] (" + e.gameTitle + "・" + (e.polarity == "pos" ? "好意的" : "否定的") + ") " + e.text;
}

std::string render(const PersonaPack& pack, const std::vector<ConditionalGap>& gaps, const std::string& label,
                   const std::vector<StanceCard>& cards, const std::vector<PackEvidence>& evidence) {
    std::vector<std::string> lines = header(pack, gaps, label);
    lines.push_back("");
    lines.push_back("# 本人の主張 (立場カード)");
    if (cards.empty()) lines.push_back("- (なし)");
    for (const auto& c : cards) lines.push_back(card_line(c));
    lines.push_back("");
    lines.push_back("# 本人の発言 (原文の抜粋)");
    for (const auto& e : evidence) lines.push_back(evidence_line(e));
    lines.push_back("");
    lines.push_back("# 代理の決まり");
    lines.push_back("- 本人の考えとして言えるのは、上の発言 [E番号] に支えられることだけ。根拠にした発言の id を必ず付ける。");
    lines.push_back("- 発言に無いことを言うときは「推測」として言い、本人がそう言ったように言わない。");
    lines.push_back("- 本人の発言に無い好みや属性 (年齢・国・名前など) を作らない。");
    return str::join(lines, "\n");
}

/** Number(id.slice(1)) (E 番号の数値部分)。読めなければ NaN。 */
double evidence_number(const std::string& id) {
    auto n = str::js_number(str::js_slice(id, 1));
    return n ? *n : std::nan("");
}

} // namespace

double estimate_tokens(std::string_view text) {
    double ascii = 0;
    for (unsigned char c : text)
        if (c < 0x80) ++ascii;
    const double total = static_cast<double>(str::js_length(text));
    return std::ceil(ascii * 0.3 + (total - ascii) * 1.3);
}

std::string dim_label(const std::string& dim) {
    const auto& labels = dim_labels();
    auto it = labels.find(dim);
    return it == labels.end() ? dim : it->second;
}

Json to_json(const ProxyBundle& b) {
    Json ids = Json::array();
    for (const auto& id : b.evidenceIds) ids.push_back(id);
    Json texts = Json::object();
    for (const auto& [k, v] : b.evidenceTexts) texts[k] = v;
    return Json{{"text", b.text},
                {"evidenceIds", ids},
                {"evidenceTexts", texts},
                {"estimatedTokens", b.estimatedTokens},
                {"droppedEvidence", b.droppedEvidence}};
}

std::vector<PackEvidence> order_evidence(const std::vector<PackEvidence>& evidence, const std::vector<std::string>& terms) {
    struct Scored {
        const PackEvidence* e;
        std::size_t i;
        int s;
    };
    std::vector<Scored> pos, neg;
    for (std::size_t i = 0; i < evidence.size(); ++i) {
        Scored x{&evidence[i], i, relevance(evidence[i], terms)};
        if (evidence[i].polarity == "pos") pos.push_back(x);
        else if (evidence[i].polarity == "neg") neg.push_back(x);
    }
    const auto by_score = [](const Scored& a, const Scored& b) { return a.s != b.s ? b.s < a.s : a.i < b.i; };
    std::stable_sort(pos.begin(), pos.end(), by_score);
    std::stable_sort(neg.begin(), neg.end(), by_score);
    std::vector<Scored> interleaved;
    for (std::size_t k = 0; k < std::max(pos.size(), neg.size()); ++k) {
        if (k < pos.size()) interleaved.push_back(pos[k]);
        if (k < neg.size()) interleaved.push_back(neg[k]);
    }
    // 議題との関係が強いものを先頭へ (安定ソート: 同点は交互の並びを保つ)。
    std::stable_sort(interleaved.begin(), interleaved.end(), [](const Scored& a, const Scored& b) { return b.s < a.s; });
    std::vector<PackEvidence> out;
    for (const auto& x : interleaved) out.push_back(*x.e);
    return out;
}

std::optional<ProxyBundle> render_proxy_bundle(const PersonaPack& pack, const BundleOptions& options) {
    if (pack.sufficiency.confidence == "low" || pack.revoked) return std::nullopt;
    const auto ordered = order_evidence(pack.evidence, options.focusTerms);
    for (std::size_t keep = ordered.size(); keep >= 1; --keep) {
        std::vector<PackEvidence> kept(ordered.begin(), ordered.begin() + static_cast<std::ptrdiff_t>(keep));
        std::set<std::string> kept_ids;
        for (const auto& e : kept) kept_ids.insert(e.id);
        std::vector<StanceCard> cards;
        for (const auto& c : pack.stanceCards) {
            StanceCard copy = c;
            copy.evidenceIds.clear();
            for (const auto& id : c.evidenceIds)
                if (kept_ids.count(id)) copy.evidenceIds.push_back(id);
            if (!copy.evidenceIds.empty()) cards.push_back(std::move(copy));
        }
        // 本文に載せる順は id 順 (読みやすさ)。どれを残すかは議題との関係で決めた。
        std::vector<PackEvidence> shown = kept;
        std::stable_sort(shown.begin(), shown.end(), [](const PackEvidence& a, const PackEvidence& b) {
            return evidence_number(a.id) < evidence_number(b.id);
        });
        std::vector<ConditionalGap> gaps;
        for (const auto& g : pack.conditionalGaps)
            if (std::all_of(g.evidenceIds.begin(), g.evidenceIds.end(), [&](const std::string& id) { return kept_ids.count(id) > 0; }))
                gaps.push_back(g);
        std::string text = render(pack, gaps, options.label, cards, shown);
        const double tokens = estimate_tokens(text);
        if (tokens <= options.budgetTokens) {
            ProxyBundle b;
            b.text = std::move(text);
            for (const auto& e : shown) {
                b.evidenceIds.push_back(e.id);
                b.evidenceTexts.emplace_back(e.id, e.text);
            }
            b.estimatedTokens = tokens;
            b.droppedEvidence = static_cast<double>(ordered.size() - keep);
            return b;
        }
    }
    return std::nullopt;
}

} // namespace di::flow::proxy
