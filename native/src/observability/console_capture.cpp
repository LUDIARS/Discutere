#include "observability/console_capture.hpp"

#include <atomic>

#include "base/log.hpp"
#include "base/strings.hpp"
#include "observability/error_buffer.hpp"

namespace di::observability {
namespace {

bool is_source_char(char c) {
    // /^([a-z0-9_-]+):/i
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

} // namespace

std::string infer_source(std::string_view message) {
    const std::string first = str::trim(message);
    // /^\[([^\]]+)\]/
    if (!first.empty() && first[0] == '[') {
        const auto close = first.find(']', 1);
        if (close != std::string::npos && close > 1) return first.substr(1, close - 1);
    }
    std::size_t i = 0;
    while (i < first.size() && is_source_char(first[i])) ++i;
    if (i > 0 && i < first.size() && first[i] == ':') return first.substr(0, i);
    return "console";
}

void install_console_capture() {
    static std::atomic<bool> installed{false};
    if (installed.exchange(true)) return;
    log::add_sink([](const log::Record& record) {
        try {
            const char* level = nullptr;
            if (record.level == log::Level::warn) level = "warn";
            else if (record.level == log::Level::error || record.level == log::Level::fatal) level = "error";
            if (!level) return;
            record_error(level, infer_source(record.message), str::trim(record.message));
        } catch (...) {
            // ロギングは本体を壊さない
        }
    });
}

} // namespace di::observability
