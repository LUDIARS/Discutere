#include "crawler/sources/website.hpp"

#include <array>
#include <stdexcept>

#include "base/log.hpp"
#include "base/strings.hpp"
#include "base/time.hpp"
#include "base/url.hpp"

namespace di::crawler::sources {

namespace {
constexpr const char* USER_AGENT = "LUDIARS-Discutere-Crawler/0.1 (+https://github.com/LUDIARS)";
constexpr const char* SOURCE = "website";

char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

/** ASCII の大文字小文字を無視して s[at..] が needle で始まるか。 */
bool istarts_at(std::string_view s, std::size_t at, std::string_view needle) {
    if (at + needle.size() > s.size()) return false;
    for (std::size_t i = 0; i < needle.size(); ++i)
        if (lower(s[at + i]) != lower(needle[i])) return false;
    return true;
}

/** 大文字小文字を無視した find (needle は短いタグ名なので O(n * |needle|))。 */
std::size_t ifind(std::string_view s, std::string_view needle, std::size_t from) {
    if (needle.empty()) return from <= s.size() ? from : std::string_view::npos;
    for (std::size_t i = from; i + needle.size() <= s.size(); ++i)
        if (lower(s[i]) == lower(needle[0]) && istarts_at(s, i, needle)) return i;
    return std::string_view::npos;
}

/** /<!--[\s\S]*?-->/g → " " */
std::string remove_comments(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    std::size_t i = 0;
    while (i < s.size()) {
        const auto at = s.find("<!--", i);
        if (at == std::string_view::npos) break;
        const auto end = s.find("-->", at + 4);
        if (end == std::string_view::npos) break;  // 以後に "-->" が無いので一致は起きない
        out.append(s.substr(i, at - i));
        out.push_back(' ');
        i = end + 3;
    }
    out.append(s.substr(i));
    return out;
}

/**
 * /<(t1|t2|...)[\s\S]*?<\/\1>/gi → " " (線形)。TS の正規表現と同じく開きタグ名の後ろの境界は見ない。
 * 閉じタグが以後に無いタグ名は、それ以降も一致しないので印を付けて飛ばす。
 */
std::string remove_blocks(std::string_view s, const std::vector<std::string>& tags) {
    std::vector<bool> no_close(tags.size(), false);
    std::string out;
    out.reserve(s.size());
    std::size_t i = 0;
    std::size_t scan = 0;
    while (scan < s.size()) {
        const auto lt = s.find('<', scan);
        if (lt == std::string_view::npos) break;
        bool matched = false;
        for (std::size_t t = 0; t < tags.size() && !matched; ++t) {
            if (no_close[t] || !istarts_at(s, lt + 1, tags[t])) continue;
            const std::string close = "</" + tags[t] + ">";
            const auto end = ifind(s, close, lt + 1 + tags[t].size());
            if (end == std::string_view::npos) {
                no_close[t] = true;
                continue;
            }
            out.append(s.substr(i, lt - i));
            out.push_back(' ');
            i = scan = end + close.size();
            matched = true;
        }
        if (!matched) scan = lt + 1;
    }
    out.append(s.substr(i));
    return out;
}

/** /<tag[^>]*>([\s\S]*?)<\/tag>/i の 1 つ目 (trim 後、空なら nullopt)。 */
std::optional<std::string> first_element(std::string_view s, std::string_view tag) {
    const auto open = ifind(s, "<" + std::string(tag), 0);
    if (open == std::string_view::npos) return std::nullopt;
    const auto gt = s.find('>', open + 1 + tag.size());
    if (gt == std::string_view::npos) return std::nullopt;
    const auto close = ifind(s, "</" + std::string(tag) + ">", gt + 1);
    if (close == std::string_view::npos) return std::nullopt;
    auto v = str::trim(s.substr(gt + 1, close - gt - 1));
    if (v.empty()) return std::nullopt;
    return v;
}

/** タグ本文 tag の at 位置に attr=["']...["'] があれば値の範囲を返す。value_re は [^"']+ 相当。 */
struct AttrHit {
    std::size_t start;  // 属性名の位置
    std::string value;
};

/** tag 内の `name=` + 引用符 + 値 ([^"']+) + 引用符 の出現をすべて返す。 */
std::vector<AttrHit> find_attrs(std::string_view tag, std::string_view name) {
    std::vector<AttrHit> hits;
    std::size_t from = 0;
    const std::string pat = std::string(name) + "=";
    while (true) {
        const auto at = ifind(tag, pat, from);
        if (at == std::string_view::npos) break;
        from = at + 1;
        std::size_t q = at + pat.size();
        if (q >= tag.size() || (tag[q] != '"' && tag[q] != '\'')) continue;
        std::size_t v = q + 1;
        std::size_t e = v;
        while (e < tag.size() && tag[e] != '"' && tag[e] != '\'') ++e;
        if (e == v || e >= tag.size()) continue;
        hits.push_back({at, std::string(tag.substr(v, e - v))});
    }
    return hits;
}

/**
 * /<meta[^>]+<key>=["']<want>["'][^>]+content=["']([^"']+)["']/i の 1 つ目のタグの値 (trim 後、空なら nullopt)。
 * 正規表現の貪欲一致と同じく、そのタグで最後に現れる有効な content を採る。
 */
std::optional<std::string> meta_content(std::string_view html, std::string_view key, std::string_view want) {
    std::size_t from = 0;
    while (true) {
        const auto at = ifind(html, "<meta", from);
        if (at == std::string_view::npos) return std::nullopt;
        from = at + 1;
        const auto gt = html.find('>', at);
        // '>' が無ければ以後のタグはすべて末尾まで続くので、ここで 1 度だけ見て終える (二乗時間を避ける)。
        if (gt == std::string_view::npos) from = html.size();
        const std::string_view tag = html.substr(at, gt == std::string_view::npos ? std::string_view::npos : gt - at);
        const auto keys = find_attrs(tag, key);
        const auto contents = find_attrs(tag, "content");
        if (keys.empty() || contents.empty()) continue;
        for (auto c = contents.rbegin(); c != contents.rend(); ++c) {
            bool ok = false;
            for (const auto& k : keys) {
                // [^>]+ が間に 1 文字以上要る。値は want と大文字小文字を無視して一致し、閉じ引用符の後に 1 文字以上。
                const std::size_t key_end = k.start + key.size() + 2 + k.value.size() + 1;
                if (k.start > 5 && str::iequals_ascii(k.value, want) && key_end < c->start) ok = true;
            }
            if (ok) {
                auto v = str::trim(c->value);
                if (v.empty()) return std::nullopt;
                return v;
            }
        }
    }
}

bool is_tracking_param(std::string_view key) {
    const std::string k = str::to_lower_ascii(key);
    return str::starts_with(k, "utm_") || k == "fbclid" || k == "gclid" || k == "yclid" || k == "mc_eid" || k == "ref" ||
           k == "ref_src";
}

/** WHATWG URL のシリアライズに合わせて非 ASCII と一部の記号を % にする。 */
std::string percent_encode_set(std::string_view s, std::string_view extra) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (c >= 0x80 || c <= 0x20 || c == 0x7F || extra.find(static_cast<char>(c)) != std::string_view::npos) {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0xF]);
        } else {
            out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

std::string detect_lang(std::string_view text) {
    std::size_t i = 0;
    while (i < text.size()) {
        const auto cp = str::next_codepoint(text, i);
        if ((cp >= 0x3040 && cp <= 0x30FF) || (cp >= 0x3400 && cp <= 0x9FFF)) return "ja";
    }
    return "en";
}
} // namespace

std::string normalize_url(std::string_view raw_url) {
    auto u = url::parse(raw_url);
    if (!u || (u->scheme != "http" && u->scheme != "https")) return str::trim(raw_url);
    std::string query = u->query;
    const auto pairs = url::parse_query(query);
    std::vector<std::pair<std::string, std::string>> kept;
    bool dropped = false;
    for (const auto& p : pairs) {
        if (is_tracking_param(p.first)) dropped = true;
        else kept.push_back(p);
    }
    // searchParams を書き換えたときだけ、クエリは application/x-www-form-urlencoded で書き直される。
    if (dropped) query = url::build_query(kept);
    else query = percent_encode_set(query, "\"<>'");
    std::string out = u->scheme + "://" + u->host_for_header();
    if (u->explicit_port) out += ":" + std::to_string(u->port);
    out += percent_encode_set(u->path, "\"<>`{}");
    if (!query.empty()) out += "?" + query;
    return out;
}

ExtractedArticle extract_article(std::string_view html) {
    // 本文に効かない領域を除去
    std::string cleaned = remove_comments(html);
    cleaned = remove_blocks(cleaned, {"script"});
    cleaned = remove_blocks(cleaned, {"style"});
    cleaned = remove_blocks(cleaned, {"noscript"});
    cleaned = remove_blocks(cleaned, {"nav", "header", "footer", "aside", "form"});

    std::optional<std::string> title = meta_content(html, "property", "og:title");
    if (!title) title = first_element(html, "title");
    if (!title) title = first_element(cleaned, "h1");

    std::optional<std::string> author = meta_content(html, "name", "author");
    if (!author) author = meta_content(html, "property", "article:author");

    // 本文候補: <article> → <main> → <body> の順で最初に見つかったもの
    std::optional<std::string> region = first_element(cleaned, "article");
    if (!region) region = first_element(cleaned, "main");
    if (!region) region = first_element(cleaned, "body");

    ExtractedArticle out;
    out.text = str::strip_html(region ? *region : cleaned);
    if (str::js_length(out.text) > MAX_CONTENT_CHARS)
        out.text = str::js_slice(out.text, 0, static_cast<std::int64_t>(MAX_CONTENT_CHARS) - 1) + "…";
    out.title = str::trim(str::decode_entities(str::strip_html(title.value_or(""))));
    if (author) out.author = str::trim(str::decode_entities(*author));
    return out;
}

ExternalUtterance map_website_article(const WebsiteMapInput& input) {
    const std::string canonical = normalize_url(input.url);
    std::string host = "web";
    if (auto u = url::parse(canonical)) {
        host = u->host;
        if (str::starts_with(host, "www.")) host = host.substr(4);
    }
    const std::string& body = input.article.text;
    const std::string content = input.article.title.empty() ? body : input.article.title + "\n\n" + body;
    const bool has_author = input.article.author && !input.article.author->empty();
    ExternalUtterance u;
    u.source = SOURCE;
    u.nativeId = canonical;  // URL を一意 id に (dedup key)
    u.gameSlug = input.gameSlug;
    u.threadKey = canonical;  // 記事 1 本 = 1 スレッド (session)
    u.content = content;
    u.lang = detect_lang(content);
    u.postedAt = input.fetchedAt;
    // 公開・安定な同一性アンカー: 署名があれば「サイト:著者」、無ければサイトドメイン
    u.authorId = has_author ? host + ":" + *input.article.author : host;
    u.authorName = has_author ? *input.article.author : host;
    u.sourceUrl = canonical;
    return u;
}

std::vector<ExternalUtterance> fetch_website_articles(const WebsiteFetchOptions& opts) {
    const long long delay = opts.politeDelayMs.value_or(1500);
    auto now = opts.now ? opts.now : [] { return static_cast<double>(time::now_ms()); };
    auto on_error = opts.onError ? opts.onError
                                 : [](const std::string& url, const std::string& err) { log::warn("\n  skip " + url + ": " + err + "\n"); };
    std::vector<ExternalUtterance> out;
    for (std::size_t i = 0; i < opts.urls.size(); ++i) {
        const std::string& target = opts.urls[i];
        try {
            http::ClientRequest req;
            req.url = target;
            req.headers.set("User-Agent", USER_AGENT);
            req.headers.set("Accept", "text/html");
            req.max_response_bytes = MAX_HTML_BYTES;
            req.timeout = std::chrono::milliseconds(30'000);
            req.allow_private_network = false;  // 利用者が指定する URL (SSRF 対策)
            auto res = do_fetch(opts.fetchImpl, req);
            if (!res.transport_ok) throw std::runtime_error(res.error);
            if (!res.ok()) throw std::runtime_error("HTTP " + std::to_string(res.status));
            auto article = extract_article(res.body);
            if (article.text.empty() || str::js_length(article.text) < 80) throw std::runtime_error("本文抽出が短すぎ (非記事ページ?)");
            out.push_back(map_website_article({target, opts.gameSlug, now(), std::move(article)}));
            if (opts.onPage) opts.onPage(out.size());
        } catch (const std::exception& e) {
            on_error(target, e.what());
        }
        if (i + 1 < opts.urls.size()) polite_sleep(delay);
    }
    return out;
}

} // namespace di::crawler::sources
