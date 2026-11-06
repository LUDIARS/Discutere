#include "flow/turn_scheduler.hpp"

namespace di::flow {

std::vector<FlowPersona> build_turn_order(const std::vector<FlowPersona>& personas, int turns_per_round, const Rng& rng) {
    std::vector<FlowPersona> speakers;
    for (const auto& p : personas)
        if (p.role != "facilitator") speakers.push_back(p);
    if (speakers.empty() || turns_per_round <= 0) return {};
    const auto target = static_cast<std::size_t>(turns_per_round);
    std::vector<FlowPersona> order;
    while (order.size() < target) {
        auto lap = shuffle(speakers, rng);
        // 周回境界の連続同一発言者を避ける (前周末尾 = 次周先頭なら 2 番手と swap)。
        if (!order.empty() && lap.size() > 1 && lap[0].id == order.back().id) std::swap(lap[0], lap[1]);
        for (auto& p : lap) order.push_back(std::move(p));
    }
    order.resize(target);
    return order;
}

} // namespace di::flow
