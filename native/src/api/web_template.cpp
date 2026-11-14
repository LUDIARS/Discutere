#include "api/web_template.hpp"

namespace di::api {

/** {{名前}} を 1 回の走査で置き換える (置き換えた値の中は再走査しない)。未知の印はそのまま残す。 */
std::string fill_template(std::string_view tpl, const std::map<std::string, std::string>& values) {
    std::string out;
    out.reserve(tpl.size() + 256);
    std::size_t i = 0;
    while (i < tpl.size()) {
        const std::size_t open = tpl.find("{{", i);
        if (open == std::string_view::npos) {
            out.append(tpl.substr(i));
            break;
        }
        const std::size_t close = tpl.find("}}", open + 2);
        if (close == std::string_view::npos) {
            out.append(tpl.substr(i));
            break;
        }
        out.append(tpl.substr(i, open - i));
        const std::string name(tpl.substr(open + 2, close - open - 2));
        auto it = values.find(name);
        if (it != values.end()) {
            out.append(it->second);
            i = close + 2;
        } else {
            out.append("{{");
            i = open + 2;
        }
    }
    return out;
}

} // namespace di::api
