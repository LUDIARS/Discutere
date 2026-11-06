#include "observability/error_buffer.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <mutex>

#include "base/strings.hpp"
#include "base/time.hpp"

namespace di::observability {
namespace {

std::mutex& buffer_mutex() {
    static std::mutex m;
    return m;
}

std::deque<BufferedError>& buffer() {
    static std::deque<BufferedError> b;
    return b;
}

} // namespace

Json to_json(const BufferedError& e) {
    return Json{{"ts", e.ts}, {"level", e.level}, {"source", e.source}, {"message", e.message}};
}

void record_error(std::string_view level, std::string_view source, std::string_view message) {
    BufferedError e;
    e.ts = time::now_ms();
    e.level = std::string(level);
    e.source = std::string(source);
    e.message = str::js_length(message) > kErrorMessageMax
                    ? str::js_slice(message, 0, static_cast<std::int64_t>(kErrorMessageMax))
                    : std::string(message);
    std::lock_guard lock(buffer_mutex());
    auto& b = buffer();
    b.push_back(std::move(e));
    while (b.size() > kErrorBufferCap) b.pop_front();
}

std::vector<BufferedError> recent_errors(double limit) {
    std::lock_guard lock(buffer_mutex());
    const auto& b = buffer();
    const double length = static_cast<double>(b.size());
    // const n = Math.max(0, Math.min(limit, buffer.length)); buffer.slice(buffer.length - n).reverse();
    std::size_t start = 0;
    if (!std::isnan(limit)) {
        const double n = std::max(0.0, std::min(limit, length));
        const double begin = std::trunc(length - n);  // slice は整数へ切り捨てる
        start = static_cast<std::size_t>(std::max(0.0, begin));
    }
    std::vector<BufferedError> out(b.begin() + static_cast<std::ptrdiff_t>(start), b.end());
    std::reverse(out.begin(), out.end());
    return out;
}

void clear_errors() {
    std::lock_guard lock(buffer_mutex());
    buffer().clear();
}

} // namespace di::observability
