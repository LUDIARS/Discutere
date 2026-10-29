#include "check.hpp"

#include <chrono>

#include "base/strings.hpp"

namespace str = di::str;

TEST_CASE("js_length and js_slice count UTF-16 units") {
    CHECK_EQ(str::js_length("abc"), std::size_t(3));
    CHECK_EQ(str::js_length("日本"), std::size_t(2));
    CHECK_EQ(str::js_length("😀"), std::size_t(2));
    CHECK_EQ(str::js_slice("日本語テキスト", 0, 3), std::string("日本語"));
    CHECK_EQ(str::js_slice("abcdef", -2), std::string("ef"));
    CHECK_EQ(str::js_slice("abcdef", 2, 1), std::string(""));
    CHECK_EQ(str::js_slice("a😀b", 0, 2), std::string("a\xEF\xBF\xBD"));  // サロゲートの片側
}

TEST_CASE("trim and squish use JS whitespace") {
    CHECK_EQ(str::trim("　 a b \n"), std::string("a b"));
    CHECK_EQ(str::squish("  a \n\t b　c  "), std::string("a b c"));
    CHECK_EQ(str::collapse_whitespace("a  b"), std::string("a b"));
}

TEST_CASE("to_lower handles ASCII and fullwidth") {
    CHECK_EQ(str::to_lower("ABC日本Ａ"), std::string("abc日本ａ"));
}

TEST_CASE("split and replace_all") {
    auto parts = str::split("a,b,,c", ",");
    CHECK_EQ(parts.size(), std::size_t(4));
    CHECK_EQ(parts[2], std::string(""));
    CHECK_EQ(str::replace_all("aaa", "a", "bb"), std::string("bbbbbb"));
    CHECK_EQ(str::split_lines("a\r\nb\nc").size(), std::size_t(3));
}

TEST_CASE("js_number follows Number()") {
    CHECK(str::js_number("") == 0.0);
    CHECK(str::js_number(" 12 ") == 12.0);
    CHECK(str::js_number("1e3") == 1000.0);
    CHECK(str::js_number("0x10") == 16.0);
    CHECK(!str::js_number("12px").has_value());
    CHECK(!str::js_number("inf").has_value());
}

TEST_CASE("strip_html matches the original regex semantics") {
    CHECK_EQ(str::strip_html("<p>Hello <b>World</b></p>"), std::string("Hello World"));
    CHECK_EQ(str::strip_html("a<script type=x>bad()</script>b"), std::string("a b"));
    CHECK_EQ(str::strip_html("a<STYLE>x{}</style>b"), std::string("a b"));
    CHECK_EQ(str::strip_html("x &amp; y &lt;z&gt; &#65;&#x42; &nbsp;q"), std::string("x & y <z> AB q"));
    CHECK_EQ(str::strip_html("a < b and c > d"), std::string("a d"));
    CHECK_EQ(str::strip_html("<scripts>keep</scripts>"), std::string("keep"));
    CHECK_EQ(str::strip_html("no close <script>x"), std::string("no close x"));
    CHECK_EQ(str::strip_html("<>"), std::string("<>"));
}

TEST_CASE("strip_html is linear on adversarial input") {
    // 移植前は 256KB で 13.8 秒 (二乗時間)。閉じない <script を大量に並べる。
    std::string evil;
    while (evil.size() < 4u << 20) evil += "<script>";
    std::string evil2;
    while (evil2.size() < 4u << 20) evil2 += "<a";
    const auto start = std::chrono::steady_clock::now();
    (void)str::strip_html(evil);
    (void)str::strip_html(evil2);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    CHECK(ms < 3000);
}

TEST_CASE("decode_entities leaves unknown and out of range alone") {
    CHECK_EQ(str::decode_entities("&foo; &#x110000; &amp"), std::string("&foo; \xEF\xBF\xBD &amp"));
}

TEST_CASE("escape_html") {
    CHECK_EQ(str::escape_html("<a href=\"x\">'&'</a>"), std::string("&lt;a href=&quot;x&quot;&gt;&#39;&amp;&#39;&lt;/a&gt;"));
}

DI_TEST_MAIN()
