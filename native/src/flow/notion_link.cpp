#include "flow/notion_link.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <stdexcept>

#include "base/json.hpp"
#include "base/sidecar.hpp"
#include "base/strings.hpp"
#include "base/url.hpp"
#include "config.hpp"

namespace di::flow {
namespace {

char lower_ascii(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool starts_with_ci(std::string_view s, std::size_t i, std::string_view word) {
    if (i + word.size() > s.size()) return false;
    for (std::size_t k = 0; k < word.size(); ++k)
        if (lower_ascii(s[i + k]) != word[k]) return false;
    return true;
}

/** [^\s<>()"'`] の否定 (URL を終える文字)。 */
bool ends_url(std::uint32_t cp) {
    return str::is_js_space(cp) || cp == '<' || cp == '>' || cp == '(' || cp == ')' || cp == '"' || cp == '\'' || cp == '`';
}

/** [.,;:!?、。」』）\]] (末尾から除く句読点)。 */
bool is_trailing_punct(std::uint32_t cp) {
    return cp == '.' || cp == ',' || cp == ';' || cp == ':' || cp == '!' || cp == '?' || cp == 0x3001 || cp == 0x3002 ||
           cp == 0x300D || cp == 0x300F || cp == 0xFF09 || cp == ']';
}

/** /(^|\.)notion\.(so|site|com)$/i */
bool is_notion_host(const std::string& host) {
    const std::string h = str::to_lower_ascii(host);
    for (const char* tail : {"notion.so", "notion.site", "notion.com"}) {
        const std::string t(tail);
        if (h == t) return true;
        if (h.size() > t.size() + 1 && str::ends_with(h, "." + t)) return true;
    }
    return false;
}

void clamp_page(NotionLinkPage& p) { p.depth = std::max(0, p.depth); }

NotionCrawlResult parse_sidecar_result(const Json& r) {
    NotionCrawlResult out;
    if (!r.is_object()) throw std::runtime_error("canalis sidecar returned invalid result");
    for (const auto& p : json_array(r, "pages")) {
        if (!p.is_object()) continue;
        NotionLinkPage page;
        page.url = json_string(p, "url");
        page.title = json_string(p, "title");
        const double d = json_number(p, "depth");
        page.depth = std::isfinite(d) ? static_cast<int>(std::clamp(d, 0.0, 64.0)) : 0;
        page.markdown = json_string(p, "markdown");
        out.pages.push_back(std::move(page));
    }
    for (const auto& w : json_array(r, "warnings"))
        if (w.is_string()) out.warnings.push_back(w.get<std::string>());
    return out;
}

int config_int(double v, int lo, int hi) {
    if (!std::isfinite(v)) return lo;
    return static_cast<int>(std::clamp(std::floor(v), static_cast<double>(lo), static_cast<double>(hi)));
}

} // namespace

std::chrono::milliseconds notion_sidecar_timeout(int max_pages) {
    const long long ms = 60'000LL + 30'000LL * std::max(1, max_pages);
    return std::chrono::milliseconds(std::min(ms, 15LL * 60'000LL));
}

NotionCrawlResult run_canalis_crawl_sidecar(const std::string& url, int depth, int max_pages) {
    sidecar::Options o;
    o.timeout = notion_sidecar_timeout(max_pages);
    o.max_output = kNotionSidecarMaxOutput;
    o.json_limits = JsonLimits{kNotionSidecarMaxOutput, 1'000'000, 16};
    const auto r = sidecar::run("canalis-crawl.ts", Json{{"url", url}, {"depth", depth}, {"maxPages", max_pages}}, o);
    if (!r.ok) throw std::runtime_error(r.error);
    return parse_sidecar_result(r.result);
}

bool is_notion_url(std::string_view url) {
    auto u = url::parse(str::trim(url));
    if (!u || (u->scheme != "http" && u->scheme != "https")) return false;
    return is_notion_host(u->host);
}

std::vector<std::string> find_notion_urls(std::string_view text) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < text.size()) {
        std::size_t head = 0;
        if (starts_with_ci(text, i, "https://")) head = 8;
        else if (starts_with_ci(text, i, "http://")) head = 7;
        if (head == 0) {
            ++i;
            continue;
        }
        // /https?:\/\/[^\s<>()"'`]+/gi: スキームの後に 1 文字以上
        std::size_t end = i + head;
        while (end < text.size()) {
            std::size_t j = end;
            if (ends_url(str::next_codepoint(text, j))) break;
            end = j;
        }
        if (end == i + head) {
            ++i;
            continue;
        }
        auto cps = str::codepoints(text.substr(i, end - i));
        while (!cps.empty() && is_trailing_punct(cps.back())) cps.pop_back();
        const std::string candidate = str::from_codepoints(cps);
        if (is_notion_url(candidate) && std::find(out.begin(), out.end(), candidate) == out.end()) out.push_back(candidate);
        i = end;
    }
    return out;
}

std::optional<int> parse_notion_depth(std::string_view text) {
    static const std::string kJa = "深さ";
    for (std::size_t i = 0; i < text.size(); ++i) {
        std::size_t p;
        if (text.compare(i, kJa.size(), kJa) == 0) p = i + kJa.size();
        else if (starts_with_ci(text, i, "depth")) p = i + 5;
        else continue;
        // \s*
        while (p < text.size()) {
            std::size_t j = p;
            if (!str::is_js_space(str::next_codepoint(text, j))) break;
            p = j;
        }
        // [:：=]
        if (p < text.size() && (text[p] == ':' || text[p] == '=')) ++p;
        else if (text.compare(p, 3, "：") == 0) p += 3;
        else continue;
        while (p < text.size()) {
            std::size_t j = p;
            if (!str::is_js_space(str::next_codepoint(text, j))) break;
            p = j;
        }
        // (\d+)
        long long value = 0;
        std::size_t digits = 0;
        while (p < text.size() && text[p] >= '0' && text[p] <= '9') {
            value = std::min<long long>(value * 10 + (text[p] - '0'), 1'000'000);
            ++p;
            ++digits;
        }
        if (digits == 0) continue;
        return static_cast<int>(std::min<long long>(MAX_NOTION_DEPTH, std::max<long long>(0, value)));
    }
    return std::nullopt;
}

std::string render_notion_pages(const std::vector<NotionLinkPage>& pages, const std::string& fallback_url) {
    std::vector<std::string> parts;
    for (auto p : pages) {
        clamp_page(p);
        parts.push_back(str::trim(str::repeat("#", static_cast<std::size_t>(std::min(6, 2 + p.depth))) + " " +
                                  (p.title.empty() ? "(無題)" : p.title) + "\n出典: " + (p.url.empty() ? fallback_url : p.url) +
                                  "\n\n" + p.markdown));
    }
    return str::join(parts, "\n\n");
}

std::string resolve_notion_url(const std::string& url, int depth, int max_pages, const ExpandNotionOpts& opts) {
    const auto& crawler = opts.crawler ? opts.crawler : NotionUrlCrawler(run_canalis_crawl_sidecar);
    auto r = crawler(url, std::clamp(depth, 0, MAX_NOTION_DEPTH), std::clamp(max_pages, 1, MAX_NOTION_PAGES));
    if (opts.warn)
        for (const auto& w : r.warnings) opts.warn(w);
    return render_notion_pages(r.pages, url);
}

std::string expand_notion_links(const std::string& text, const ExpandNotionOpts& opts) {
    auto urls = find_notion_urls(text);
    if (urls.empty()) return "";
    if (urls.size() > MAX_NOTION_LINKS) {
        if (opts.warn)
            opts.warn("Notion リンクが多すぎるため先頭 " + std::to_string(MAX_NOTION_LINKS) + " 件だけ取得します (" +
                      std::to_string(urls.size()) + " 件)");
        urls.resize(MAX_NOTION_LINKS);
    }
    const auto& cfg = config::get_config().flow.notionLinks;
    int depth;
    if (opts.depth) depth = *opts.depth;
    else if (auto d = parse_notion_depth(text)) depth = *d;
    else depth = config_int(cfg.maxDepth, 0, MAX_NOTION_DEPTH);
    const int max_pages = opts.maxPages ? *opts.maxPages : config_int(cfg.maxPages, 1, MAX_NOTION_PAGES);
    std::vector<std::string> parts;
    for (const auto& url : urls) {
        try {
            std::string md = resolve_notion_url(url, depth, max_pages, opts);
            if (!md.empty()) parts.push_back(std::move(md));
        } catch (const std::exception& e) {
            if (opts.warn) opts.warn("Notion リンク取得失敗 (" + url + "): " + e.what());
        }
    }
    return parts.empty() ? "" : "# Notion 資料\n\n" + str::join(parts, "\n\n");
}

} // namespace di::flow
