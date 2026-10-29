#include "lib/sentiment_core/design_gap.hpp"

namespace di::lib::sentiment_core {

namespace {
std::optional<std::size_t> index_of(const std::vector<std::string>& keys, const std::string& key) {
    for (std::size_t i = 0; i < keys.size(); ++i)
        if (keys[i] == key) return i;
    return std::nullopt;
}
} // namespace

Vector build_target_vector(const std::vector<MechanicIntent>& mechanics) {
    Vector target(DIM, 0.5);
    const auto& emo = emo_keys();
    const auto& asp = asp_keys();
    for (const auto& m : mechanics) {
        if (m.intended_aspects)
            for (const auto& a : *m.intended_aspects)
                if (auto i = index_of(asp, a)) target[2 + emo.size() + *i] = 1;
        if (m.intended_emotions)
            for (const auto& e : *m.intended_emotions)
                if (auto i = index_of(emo, e)) target[2 + *i] = 1;
    }
    const std::size_t valence_index = 0;  // VECTOR_SPEC.indexOf("emo.valence")
    double sum = 0;
    double count = 0;
    for (const auto& m : mechanics) {
        if (!m.intended_valence) continue;
        const std::string& v = *m.intended_valence;
        sum += v == "positive" ? 1 : v == "negative" ? -1 : 0;
        count += 1;
    }
    target[valence_index] = count == 0 ? 1.0 : (sum / count + 1) / 2;
    return target;
}

Vector compute_design_gap(const Vector& current, const Vector& target) { return subtract(target, current); }

} // namespace di::lib::sentiment_core
