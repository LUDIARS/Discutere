// Lorica が、この翻訳単位に実際に掛かっていることを、翻訳単位の中から確かめる (ctest の lorica_applied.baseline /
// lorica_applied.server)。同じソースを di_harden(<t>) と di_harden(<t> SERVER) の 2 通りで作る (tests/lorica_applied/CMakeLists.txt)。
// 期待値はコンパイル定義で受ける:
//   DI_LORICA_EXPECT_EXTENDED     1 = EXTENDED の印を求める、0 = baseline の印を求める
//   DI_LORICA_EXPECT_FORTIFY      Lorica がこの構成で付ける _FORTIFY_SOURCE の値 (Debug など付けない構成は 0)
//   DI_LORICA_EXPECT_UNSAFE_API   1 = lorica/unsafe_api.hpp の強制 include (MSVC は /we4996) を求める
// 引数 --skip-baseline <理由> を受けたとき (EXTENDED を確かめるテストなのに、サーバが baseline に下がっている構成) は、
// baseline の確認だけをして終了コード 77 (ctest の SKIP_RETURN_CODE) で「確かめていない」と返す。通過にはしない。

// ---- ここから先、何かを #include する前に見る ----
// lorica_deprecate_unsafe_apis は GCC / Clang で lorica/unsafe_api.hpp を、この行より前に強制 include する。
// そのヘッダは #pragma once で、自分のマクロを最後に #undef するので、残る印は (1) strcpy などの宣言そのものと、
// (2) glibc では features.h を先に読むこと (_FEATURES_H)。ここではまだ何も include していないので、どちらも
// 強制 include が無ければ現れない。strcpy の宣言は、依存名の呼び出しを requires で試す
// (Char が基本型なので ADL は何も探さず、この定義より前に見えた宣言だけが当たる)。
#if defined(_WIN32)
// Windows: Lorica はヘッダを差し込まず、UCRT 自身の deprecation (C4996) を /we4996 でエラーにするだけ。
// 翻訳単位の中から見える印は無いので、ここでは見ない (lorica_applied.compile_commands が /we4996 を見る)。
#define DI_LORICA_CAN_SEE_UNSAFE_API_HEADER 0
#else
#define DI_LORICA_CAN_SEE_UNSAFE_API_HEADER 1
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
// requires の中の式は評価されない (宣言があるかを見るだけで、呼んではいない)。名前は lorica/unsafe_api.hpp 自身と同じく
// マクロで綴り、呼び出しに見える行を置かない (Praesidium の文字列の規則 native.unbounded-copy は呼び出しの形を探す)。
#define DI_LORICA_PROBE_UNSAFE_NAME(a, b) a##b
template <class Char>
constexpr bool di_lorica_strcpy_declared_first = requires(Char* destination, const Char* source) {
    DI_LORICA_PROBE_UNSAFE_NAME(str, cpy)(destination, source);
};
#undef DI_LORICA_PROBE_UNSAFE_NAME
inline constexpr bool di_lorica_seen_strcpy_declaration = di_lorica_strcpy_declared_first<char>;
#pragma GCC diagnostic pop
#if defined(__has_include)
#if __has_include(<gnu/libc-version.h>)
#define DI_LORICA_GLIBC 1
#endif
#endif
#if defined(DI_LORICA_GLIBC) && defined(_FEATURES_H)
inline constexpr bool di_lorica_seen_features_h = true;
#else
inline constexpr bool di_lorica_seen_features_h = false;
#endif
#endif
// ---- ここまで ----

#include "check.hpp"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include <lorica/checked_integer.hpp>
#include <lorica/input_budget.hpp>

#if !defined(DI_LORICA_EXPECT_EXTENDED) || !defined(DI_LORICA_EXPECT_FORTIFY) || !defined(DI_LORICA_EXPECT_UNSAFE_API)
#error "tests/lorica_applied/CMakeLists.txt が期待値 (DI_LORICA_EXPECT_*) を渡していない"
#endif

#define DI_LORICA_STR2(x) #x
#define DI_LORICA_STR(x) DI_LORICA_STR2(x)

namespace {

bool g_unsafe_api_unchecked = false;

TEST_CASE("unsafe API rejection: lorica/unsafe_api.hpp is force-included") {
#if DI_LORICA_EXPECT_UNSAFE_API && DI_LORICA_CAN_SEE_UNSAFE_API_HEADER
    CHECK(di_lorica_seen_strcpy_declaration);
#if defined(DI_LORICA_GLIBC)
    CHECK(di_lorica_seen_features_h);
#endif
#elif DI_LORICA_EXPECT_UNSAFE_API
    // MSVC: 上の注記のとおり、翻訳単位の中からは見えない。
    g_unsafe_api_unchecked = true;
#else
    // この構成では Lorica が拒否を掛けられない (configure の STATUS に理由)。掛かっていないことだけを見る。
    g_unsafe_api_unchecked = true;
#if DI_LORICA_CAN_SEE_UNSAFE_API_HEADER
    CHECK(!di_lorica_seen_strcpy_declaration);
#endif
#endif
}

TEST_CASE("standard library hardening matches the Lorica profile") {
#if defined(_MSC_VER) && !defined(__clang__)
    // MSVC: baseline は /GS /sdl /guard:cf (マクロにならないので compile_commands の側で見る)。EXTENDED は STL の定義。
#if DI_LORICA_EXPECT_EXTENDED
#if defined(_MSVC_STL_HARDENING) && defined(_MSVC_STL_DESTRUCTOR_TOMBSTONES)
    CHECK_EQ(_MSVC_STL_HARDENING, 1);
    CHECK_EQ(_MSVC_STL_DESTRUCTOR_TOMBSTONES, 1);
#else
    CHECK(!"_MSVC_STL_HARDENING / _MSVC_STL_DESTRUCTOR_TOMBSTONES are not defined");
#endif
#endif
#else
    // GCC / Clang (Linux): Lorica は C++ に -D_GLIBCXX_ASSERTIONS と -D_LIBCPP_HARDENING_MODE=<FAST か EXTENSIVE> を両方付ける。
#if defined(_GLIBCXX_ASSERTIONS)
    const bool glibcxx_assertions = true;
#else
    const bool glibcxx_assertions = false;
#endif
    CHECK(glibcxx_assertions);
#if !defined(_LIBCPP_HARDENING_MODE)
    CHECK(!"_LIBCPP_HARDENING_MODE is not defined");
#elif defined(_LIBCPP_VERSION)
    // libc++: 値で比べる。
#if DI_LORICA_EXPECT_EXTENDED
    CHECK(_LIBCPP_HARDENING_MODE == _LIBCPP_HARDENING_MODE_EXTENSIVE);
#else
    CHECK(_LIBCPP_HARDENING_MODE == _LIBCPP_HARDENING_MODE_FAST);
#endif
#else
    // libstdc++: _LIBCPP_HARDENING_MODE_* は定義されないので、マクロの中身 (トークン) を文字列で比べる。
    // Lorica はこれを対象の LORICA_HARDENING_PROFILE で切り替える (EXTENDED の、翻訳単位から見える印)。
    const std::string_view mode = DI_LORICA_STR(_LIBCPP_HARDENING_MODE);
#if DI_LORICA_EXPECT_EXTENDED
    CHECK_EQ(mode, std::string_view("_LIBCPP_HARDENING_MODE_EXTENSIVE"));
#else
    CHECK_EQ(mode, std::string_view("_LIBCPP_HARDENING_MODE_FAST"));
#endif
#endif
#endif
}

TEST_CASE("_FORTIFY_SOURCE has the level Lorica selects for this configuration") {
#if defined(_MSC_VER) && !defined(__clang__)
    // MSVC に _FORTIFY_SOURCE は無い (Lorica も付けない)。
#elif DI_LORICA_EXPECT_FORTIFY > 0
#if defined(_FORTIFY_SOURCE)
    CHECK_EQ(_FORTIFY_SOURCE, DI_LORICA_EXPECT_FORTIFY);
#else
    CHECK(!"_FORTIFY_SOURCE is not defined");
#endif
#if defined(__USE_FORTIFY_LEVEL)
    // glibc が実際に効かせている段 (最適化なしや、ヘッダが受け付けない値なら下がる)。
    CHECK_EQ(__USE_FORTIFY_LEVEL, DI_LORICA_EXPECT_FORTIFY);
#endif
#else
    // Debug などの最適化なしの構成には Lorica は FORTIFY を付けない (LoricaHardening.cmake の構成の条件)。
#endif
}

TEST_CASE("Lorica::boundary: checked_add reports overflow") {
    constexpr auto max = std::numeric_limits<std::uint32_t>::max();
    CHECK(!lorica::checked_add(max, std::uint32_t{1}).has_value());
    CHECK_EQ(lorica::checked_add(max - 1, std::uint32_t{1}).value_or(0), max);
    CHECK(!lorica::checked_add(std::numeric_limits<std::int64_t>::min(), std::int64_t{-1}).has_value());
}

TEST_CASE("Lorica::boundary: input_budget admits the limit and refuses limit + 1") {
    constexpr std::size_t limit = 1024;
    {
        lorica::input_budget exact({limit, 10, 10, 4});
        CHECK(exact.consume(limit, 10, 10));
        CHECK(!exact.consume(1, 0, 0));
        CHECK(exact.permits_depth(4));
        CHECK(!exact.permits_depth(5));
    }
    {
        lorica::input_budget over({limit, 10, 10, 4});
        CHECK(!over.consume(limit + 1, 0, 0));
        CHECK(!over.consume(0, 11, 0));
        CHECK(!over.consume(0, 0, 11));
        // 断った分は引かれていない。
        CHECK(over.consume(limit, 10, 10));
    }
}

} // namespace

int main(int argc, char** argv) {
    const std::vector<std::string_view> args(argv + 1, argv + argc);
    const int result = di::test::run_all();
    if (g_unsafe_api_unchecked)
        std::cout << "note: unsafe API rejection is not checked inside this unit (see the comments and configure STATUS)\n";
    if (args.size() == 2 && args[0] == "--skip-baseline") {
        if (result != 0)
            return result;
        std::cout << "SKIP: Lorica EXTENDED を確かめるテストだが、サーバは baseline に下がっている: " << args[1] << '\n';
        return 77;
    }
    if (!args.empty()) {
        std::cout << "unknown arguments\n";
        return 2;
    }
    return result;
}
