#pragma once
// ログの error / warn を監視して error-buffer に積む (src/observability/console-capture.ts の移植)。
//
// TS は console.error / console.warn を monkeypatch していた。C++ では di::log の sink として登録する
// (元の標準出力・標準エラーへの出力はそのまま)。監視自体が落ちないよう例外は握りつぶす。
// fatal は TS に無い水準なので error として積む。
#include <string>
#include <string_view>

namespace di::observability {

/** 1 度だけ sink を登録する (二重適用は no-op)。 */
void install_console_capture();

/**
 * 先頭の "[foo] ..." / "foo:" のような prefix を source として推測する。推測できなければ "console"。
 * (TS の inferSource。テスト用に公開。)
 */
std::string infer_source(std::string_view message);

} // namespace di::observability
