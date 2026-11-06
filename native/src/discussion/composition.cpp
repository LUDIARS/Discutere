#include "discussion/composition.hpp"

#include <algorithm>
#include <cmath>
#include <set>

namespace di::discussion {

namespace {
const std::vector<std::string> NAME_POOL = {"陽介", "凛", "ハル", "美咲", "翔", "葵", "蓮", "結衣", "拓海", "彩",
                                            "悠真", "詩織", "大樹", "舞", "颯太", "真央", "啓介", "杏奈", "亮", "千夏"};

struct Flavor {
    std::string style;
    std::vector<std::string> traits;
};

const Flavor& tier_flavor(const Tier& tier) {
    static const Flavor facilitator{"穏やかな進行役口調", {"中立", "俯瞰", "巻き込み上手"}};
    static const Flavor keyman{"芯のある説得口調", {"論点を掴む", "具体例豊富", "粘り強い"}};
    static const Flavor opinion{"自然な雑談口調", {"独自の角度", "素直", "発想が柔らかい"}};
    if (tier == "facilitator") return facilitator;
    if (tier == "keyman") return keyman;
    return opinion;
}

/** Math.floor(rng() * n) を [0, n) に収める (rng が範囲外の値を返しても落ちない)。 */
std::size_t pick_index(const Rng& rng, std::size_t n) {
    const double v = std::floor(rng() * static_cast<double>(n));
    if (!(v >= 0)) return 0;
    return std::min(static_cast<std::size_t>(v), n - 1);
}

/** 未使用の名前を 1 つ引く (尽きたら index サフィックスで一意化)。used は Set の挿入順を保つ必要がないので set。 */
std::string pick_name(std::set<std::string>& used, const Rng& rng) {
    std::vector<std::string> free;
    for (const auto& n : NAME_POOL)
        if (!used.count(n)) free.push_back(n);
    const auto& pool = free.empty() ? NAME_POOL : free;
    const std::string name = pool[pick_index(rng, pool.size())];
    const std::string final_name = used.count(name) ? name + std::to_string(used.size()) : name;
    used.insert(final_name);
    return final_name;
}

PartyMember make_member(std::string slot_id, Tier tier, std::string model, std::set<std::string>& used, const Rng& rng) {
    const Flavor& f = tier_flavor(tier);
    PartyMember m;
    m.slotId = std::move(slot_id);
    m.tier = std::move(tier);
    m.model = std::move(model);
    m.personaName = pick_name(used, rng);
    m.speechStyle = f.style;
    m.traits = f.traits;
    return m;
}
} // namespace

Json to_json(const PartyMember& m) {
    return Json{{"slotId", m.slotId},           {"tier", m.tier},           {"model", m.model},
                {"personaName", m.personaName}, {"speechStyle", m.speechStyle}, {"traits", m.traits}};
}

int party_size(double expected_utterances, int min_total) {
    return std::max(min_total, static_cast<int>(std::ceil(expected_utterances / 4)) + 1);
}

std::string pick_weighted_model(const std::vector<WeightedModel>& models, const Rng& rng) {
    double total = 0;
    for (const auto& m : models) total += std::max(0.0, m.weight);
    if (total <= 0) return models.empty() ? std::string() : models[0].model;
    double r = rng() * total;
    for (const auto& m : models) {
        r -= std::max(0.0, m.weight);
        if (r < 0) return m.model;
    }
    return models.back().model;
}

std::vector<PartyMember> form_party(const PartyConfig& cfg, const Rng& rng) {
    const int total = party_size(cfg.expectedUtterances, cfg.minTotal);
    const int opinion_count = std::max(1, total - 1 - cfg.keymanCount);
    std::set<std::string> used;
    std::vector<PartyMember> members;
    members.push_back(make_member("facilitator", "facilitator", cfg.facilitatorModel, used, rng));
    for (int i = 1; i <= cfg.keymanCount; ++i)
        members.push_back(make_member("keyman-" + std::to_string(i), "keyman", cfg.keymanModel, used, rng));
    for (int i = 1; i <= opinion_count; ++i)
        members.push_back(make_member("opinion-" + std::to_string(i), "opinion", pick_weighted_model(cfg.opinionModels, rng), used, rng));
    return members;
}

std::vector<PartyMember> swap_half(const std::vector<PartyMember>& party, const PartyConfig& cfg, const Rng& rng) {
    std::vector<const PartyMember*> swappable;
    for (const auto& m : party)
        if (m.tier != "facilitator") swappable.push_back(&m);
    const std::size_t swap_count = swappable.size() / 2;
    // 入替対象スロットをランダムに swap_count 個選ぶ (Fisher-Yates 部分シャッフル)。
    std::vector<std::size_t> idx(swappable.size());
    for (std::size_t i = 0; i < idx.size(); ++i) idx[i] = i;
    for (std::size_t i = idx.size(); i-- > 1;) std::swap(idx[i], idx[pick_index(rng, i + 1)]);
    std::set<std::string> swap_slots;
    for (std::size_t k = 0; k < swap_count; ++k) swap_slots.insert(swappable[idx[k]]->slotId);

    std::set<std::string> used;
    for (const auto& m : party)
        if (!swap_slots.count(m.slotId)) used.insert(m.personaName);
    std::vector<PartyMember> out;
    for (const auto& m : party) {
        if (!swap_slots.count(m.slotId)) {
            out.push_back(m);
            continue;
        }
        const std::string model = m.tier == "keyman" ? cfg.keymanModel : pick_weighted_model(cfg.opinionModels, rng);
        out.push_back(make_member(m.slotId, m.tier, model, used, rng));
    }
    return out;
}

Stance decide_stance(const PartyMember& member, const Rng& rng) {
    if (member.tier == "facilitator") return "neutral";
    return rng() < 0.5 ? "pro" : "con";
}

} // namespace di::discussion
