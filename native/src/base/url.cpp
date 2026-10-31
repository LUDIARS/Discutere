#include "base/url.hpp"

#include "base/strings.hpp"

namespace di::url {

std::string Url::host_for_header() const {
    if (host.find(':') != std::string::npos) return "[" + host + "]";
    return host;
}

std::string Url::origin() const {
    std::string out = scheme + "://" + host_for_header();
    if (explicit_port) out += ":" + std::to_string(port);
    return out;
}

std::optional<Url> parse(std::string_view text) {
    Url u;
    const auto sep = text.find("://");
    if (sep == std::string_view::npos || sep == 0) return std::nullopt;
    u.scheme = str::to_lower_ascii(text.substr(0, sep));
    std::uint16_t default_port;
    if (u.scheme == "http" || u.scheme == "ws") default_port = 80;
    else if (u.scheme == "https" || u.scheme == "wss") default_port = 443;
    else return std::nullopt;
    std::string_view rest = text.substr(sep + 3);
    const auto end_authority = rest.find_first_of("/?#");
    std::string_view authority = rest.substr(0, end_authority);
    std::string_view tail = end_authority == std::string_view::npos ? std::string_view{} : rest.substr(end_authority);
    if (authority.empty() || authority.find('@') != std::string_view::npos) return std::nullopt;
    for (char c : authority) {
        const auto uc = static_cast<unsigned char>(c);
        if (uc <= 0x20 || uc == 0x7F || c == '\\') return std::nullopt;
    }
    std::string_view host;
    std::string_view port;
    if (authority.front() == '[') {
        const auto close = authority.find(']');
        if (close == std::string_view::npos) return std::nullopt;
        host = authority.substr(1, close - 1);
        auto after = authority.substr(close + 1);
        if (!after.empty()) {
            if (after.front() != ':') return std::nullopt;
            port = after.substr(1);
        }
    } else {
        const auto colon = authority.rfind(':');
        if (colon != std::string_view::npos) {
            host = authority.substr(0, colon);
            port = authority.substr(colon + 1);
        } else {
            host = authority;
        }
    }
    if (host.empty()) return std::nullopt;
    u.host = str::to_lower_ascii(host);
    u.port = default_port;
    if (!port.empty()) {
        auto p = str::parse_int(port);
        if (!p || *p <= 0 || *p > 65535) return std::nullopt;
        if (*p != default_port) {
            u.port = static_cast<std::uint16_t>(*p);
            u.explicit_port = true;
        }
    }
    const auto hash = tail.find('#');
    if (hash != std::string_view::npos) {
        u.fragment = std::string(tail.substr(hash + 1));
        tail = tail.substr(0, hash);
    }
    const auto q = tail.find('?');
    u.path = std::string(q == std::string_view::npos ? tail : tail.substr(0, q));
    if (u.path.empty()) u.path = "/";
    if (q != std::string_view::npos) u.query = std::string(tail.substr(q + 1));
    u.target = u.path + (u.query.empty() ? "" : "?" + u.query);
    for (char c : u.target) {
        const auto uc = static_cast<unsigned char>(c);
        if (uc < 0x20 || uc == 0x7F || c == ' ') return std::nullopt;  // 要求行の注入を防ぐ
    }
    return u;
}

std::string encode_component(std::string_view s) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
                                c == '_' || c == '.' || c == '!' || c == '~' || c == '*' || c == '\'' || c == '(' || c == ')';
        if (unreserved) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0xF]);
        }
    }
    return out;
}

namespace {
int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
} // namespace

std::optional<std::string> decode_component(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '%') {
            out.push_back(s[i]);
            continue;
        }
        if (i + 2 >= s.size()) return std::nullopt;
        const int hi = hexval(s[i + 1]);
        const int lo = hexval(s[i + 2]);
        if (hi < 0 || lo < 0) return std::nullopt;
        out.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
    }
    if (!str::is_valid_utf8(out)) return std::nullopt;
    return out;
}

std::string decode_form(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+') {
            out.push_back(' ');
        } else if (s[i] == '%' && i + 2 < s.size() && hexval(s[i + 1]) >= 0 && hexval(s[i + 2]) >= 0) {
            out.push_back(static_cast<char>((hexval(s[i + 1]) << 4) | hexval(s[i + 2])));
            i += 2;
        } else {
            out.push_back(s[i]);
        }
    }
    return str::sanitize_utf8(out);
}

std::vector<std::pair<std::string, std::string>> parse_query(std::string_view query) {
    std::vector<std::pair<std::string, std::string>> out;
    if (!query.empty() && query.front() == '?') query.remove_prefix(1);
    for (const auto& part : str::split(query, "&")) {
        if (part.empty()) continue;
        const auto eq = part.find('=');
        if (eq == std::string::npos) out.emplace_back(decode_form(part), "");
        else out.emplace_back(decode_form(part.substr(0, eq)), decode_form(part.substr(eq + 1)));
    }
    return out;
}

std::string build_query(const std::vector<std::pair<std::string, std::string>>& params) {
    auto enc = [](std::string_view s) {
        static constexpr char hex[] = "0123456789ABCDEF";
        std::string out;
        for (unsigned char c : s) {
            const bool keep = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                              c == '.' || c == '*';
            if (keep) out.push_back(static_cast<char>(c));
            else if (c == ' ') out.push_back('+');
            else {
                out.push_back('%');
                out.push_back(hex[c >> 4]);
                out.push_back(hex[c & 0xF]);
            }
        }
        return out;
    };
    std::string out;
    for (const auto& [k, v] : params) {
        if (!out.empty()) out.push_back('&');
        out += enc(k) + "=" + enc(v);
    }
    return out;
}

} // namespace di::url
