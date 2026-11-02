#include "core/vectors/vector_math.hpp"

#include <cmath>

namespace di::core::vectors {

bool is_embedding_vector(const Json& value) {
    if (!value.is_array() || value.empty()) return false;
    for (const auto& item : value) {
        if (!item.is_number()) return false;
        if (!std::isfinite(item.get<double>())) return false;
    }
    return true;
}

std::optional<std::vector<double>> to_embedding_vector(const Json& value) {
    if (!is_embedding_vector(value)) return std::nullopt;
    std::vector<double> out;
    out.reserve(value.size());
    for (const auto& item : value) out.push_back(item.get<double>());
    return out;
}

double cosine_similarity(const std::vector<double>& a, const std::vector<double>& b) {
    if (a.empty() || a.size() != b.size()) return 0;
    double dot = 0, na = 0, nb = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        dot += a[i] * b[i];
        na += a[i] * a[i];
        nb += b[i] * b[i];
    }
    if (na <= 0 || nb <= 0) return 0;
    return dot / (std::sqrt(na) * std::sqrt(nb));
}

} // namespace di::core::vectors
