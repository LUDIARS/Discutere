#pragma once
// libFuzzer 形式の入口 (LLVMFuzzerTestOneInput) の共通宣言。
// - DI_FUZZ=ON (Clang) では libFuzzer がこれを呼ぶ (di_fuzz_<name>)。
// - いつものビルド (GCC を含む) では regress_main.cpp が固定の入力で 1 回ずつ呼ぶ (ctest の fuzz.<name>)。
// 入口は外からのバイト列を受けるだけで、上限・壊れた入力は対象の関数が断る。落ちたら (sanitizer の検出を含む) 失敗。
#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);
