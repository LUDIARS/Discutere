# fuzz の入口 (libFuzzer 形式 LLVMFuzzerTestOneInput)。
# - いつも: 回帰用の実行ファイル di_fuzz_regress_<name> (固定の入力で 1 回ずつ呼ぶ) を ctest の fuzz.<name> に登録する。
# - DI_FUZZ=ON (Clang のみ): libFuzzer の実行ファイル di_fuzz_<name> も作る。例:
#     cmake -S native -B build-fuzz -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
#       -DCMAKE_BUILD_TYPE=Debug -DDI_SANITIZE=address,undefined -DDI_FUZZ=ON
#     build-fuzz/tests/fuzz/di_fuzz_json -max_total_time=30 native/tests/fuzz/corpus/json
set(DI_FUZZ_ENTRIES json gateway_frame claude_cli_result s3_xml front_matter)
foreach(name ${DI_FUZZ_ENTRIES})
  set(source ${CMAKE_CURRENT_SOURCE_DIR}/fuzz_${name}.cpp)
  add_executable(di_fuzz_regress_${name} ${source} ${CMAKE_CURRENT_SOURCE_DIR}/regress_main.cpp)
  target_link_libraries(di_fuzz_regress_${name} PRIVATE di_app)
  di_target_warnings(di_fuzz_regress_${name})
  di_harden(di_fuzz_regress_${name})
  add_test(NAME fuzz.${name} COMMAND di_fuzz_regress_${name} ${CMAKE_CURRENT_SOURCE_DIR}/corpus/${name})
  set_tests_properties(fuzz.${name} PROPERTIES TIMEOUT 120)
  if(DI_FUZZ)
    add_executable(di_fuzz_${name} ${source})
    target_link_libraries(di_fuzz_${name} PRIVATE di_app)
    target_link_options(di_fuzz_${name} PRIVATE -fsanitize=fuzzer)
    di_target_warnings(di_fuzz_${name})
    di_harden(di_fuzz_${name})
  endif()
endforeach()
