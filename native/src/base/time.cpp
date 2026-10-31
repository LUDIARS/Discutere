#include "base/time.hpp"

#include <cstdio>

namespace di::time {
namespace {
// Howard Hinnant の days_from_civil / civil_from_days。
std::int64_t days_from_civil(std::int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}
void civil_from_days(std::int64_t z, int& y, int& m, int& d) {
    z += 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t yy = static_cast<std::int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    y = static_cast<int>(yy + (m <= 2));
}
std::int64_t floor_div(std::int64_t a, std::int64_t b) { return a / b - ((a % b != 0) && ((a < 0) != (b < 0))); }

bool read_digits(std::string_view s, std::size_t& i, std::size_t count, int& out) {
    if (i + count > s.size()) return false;
    int v = 0;
    for (std::size_t k = 0; k < count; ++k) {
        const char c = s[i + k];
        if (c < '0' || c > '9') return false;
        v = v * 10 + (c - '0');
    }
    out = v;
    i += count;
    return true;
}
} // namespace

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::int64_t steady_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::int64_t from_civil_utc(int year, int month, int day, int hour, int minute, int second, int millis) {
    const std::int64_t days = days_from_civil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
    return ((days * 24 + hour) * 60 + minute) * 60000 + static_cast<std::int64_t>(second) * 1000 + millis;
}

Civil to_civil_utc(std::int64_t ms) {
    const std::int64_t days = floor_div(ms, 86'400'000);
    std::int64_t rem = ms - days * 86'400'000;
    Civil c{};
    civil_from_days(days, c.year, c.month, c.day);
    c.hour = static_cast<int>(rem / 3'600'000);
    rem %= 3'600'000;
    c.minute = static_cast<int>(rem / 60'000);
    rem %= 60'000;
    c.second = static_cast<int>(rem / 1000);
    c.millis = static_cast<int>(rem % 1000);
    return c;
}

std::string iso_utc(std::int64_t ms) {
    const Civil c = to_civil_utc(ms);
    char buf[40];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", c.year, c.month, c.day, c.hour, c.minute, c.second,
                  c.millis);
    return buf;
}

std::string now_iso() { return iso_utc(now_ms()); }

std::string ymd_utc(std::int64_t ms) {
    const Civil c = to_civil_utc(ms);
    char buf[16];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02d", c.year, c.month, c.day);
    return buf;
}

std::string sqlite_datetime_utc(std::int64_t ms) {
    const Civil c = to_civil_utc(ms);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02d %02d:%02d:%02d", c.year, c.month, c.day, c.hour, c.minute, c.second);
    return buf;
}

std::optional<std::int64_t> parse_iso(std::string_view s) {
    std::size_t i = 0;
    int y, mo, d, h = 0, mi = 0, se = 0, ms = 0;
    if (!read_digits(s, i, 4, y) || i >= s.size() || s[i++] != '-') return std::nullopt;
    if (!read_digits(s, i, 2, mo) || i >= s.size() || s[i++] != '-') return std::nullopt;
    if (!read_digits(s, i, 2, d)) return std::nullopt;
    if (mo < 1 || mo > 12 || d < 1 || d > 31) return std::nullopt;
    std::int64_t offset_ms = 0;
    if (i < s.size()) {
        if (s[i] != 'T' && s[i] != ' ' && s[i] != 't') return std::nullopt;
        ++i;
        if (!read_digits(s, i, 2, h) || i >= s.size() || s[i++] != ':') return std::nullopt;
        if (!read_digits(s, i, 2, mi)) return std::nullopt;
        if (i < s.size() && s[i] == ':') {
            ++i;
            if (!read_digits(s, i, 2, se)) return std::nullopt;
            if (i < s.size() && s[i] == '.') {
                ++i;
                std::size_t start = i;
                int frac = 0;
                std::size_t digits = 0;
                while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
                    if (digits < 3) frac = frac * 10 + (s[i] - '0');
                    ++digits;
                    ++i;
                }
                if (i == start) return std::nullopt;
                while (digits < 3) {
                    frac *= 10;
                    ++digits;
                }
                ms = frac;
            }
        }
        if (i < s.size()) {
            if (s[i] == 'Z' || s[i] == 'z') {
                ++i;
            } else if (s[i] == '+' || s[i] == '-') {
                const int sign = s[i] == '-' ? -1 : 1;
                ++i;
                int oh, om = 0;
                if (!read_digits(s, i, 2, oh)) return std::nullopt;
                if (i < s.size() && s[i] == ':') ++i;
                if (i < s.size() && !read_digits(s, i, 2, om)) return std::nullopt;
                offset_ms = sign * (static_cast<std::int64_t>(oh) * 3'600'000 + static_cast<std::int64_t>(om) * 60'000);
            }
        }
        if (i != s.size()) return std::nullopt;
        if (h > 24 || mi > 59 || se > 60) return std::nullopt;
    }
    return from_civil_utc(y, mo, d, h, mi, se, ms) - offset_ms;
}

} // namespace di::time
