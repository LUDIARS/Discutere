# Lorica が実際に掛かっていることを、ビルドの結果から確かめるテスト (job cpp-lorica-20261111、COMMON-4 作業 B)。
# 取り込みが外れた・守りのフラグが落ちた・強制 include が効いていない、を ctest が落ちることで分かるようにする。
#
#   lorica_applied.baseline          di_harden(<t>) で作った翻訳単位の中から、baseline の印と Lorica::boundary を見る
#   lorica_applied.server            di_harden(<t> SERVER) で作った翻訳単位の中から、EXTENDED の印を見る
#                                    (DI_LORICA_PROFILE=auto で baseline に下がった構成では、その旨を出して skip)
#   lorica_applied.compile_commands  compile_commands.json の全エントリ (Discutere のビルド単位) のフラグを見る
#   lorica_applied.verify_binary     Lorica の tools/verify_binary.py を discutere-server に掛ける (python3 があるときだけ)

# Lorica が、この構成で付ける _FORTIFY_SOURCE の値 (LoricaHardening.cmake の _lorica_linux_capabilities と同じ規則:
# Release / RelWithDebInfo / MinSizeRel だけ、コンパイラが 12 以上なら 3、それより前は 2)。
if(CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 12)
  set(_di_lorica_fortify 3)
else()
  set(_di_lorica_fortify 2)
endif()
if(MSVC)
  set(_di_lorica_fortify 0)
endif()
if(DI_LORICA_UNSAFE_API_REJECTION)
  set(_di_lorica_unsafe 1)
else()
  set(_di_lorica_unsafe 0)
endif()

function(_di_lorica_applied_check target expect_extended)
  add_executable(${target} ${CMAKE_CURRENT_SOURCE_DIR}/lorica_applied_check.cpp)
  target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
  # check.hpp が base/env.hpp を使う。Lorica::boundary は di_harden が付ける。
  target_link_libraries(${target} PRIVATE di_base)
  target_compile_definitions(${target} PRIVATE
    DI_LORICA_EXPECT_EXTENDED=${expect_extended}
    "DI_LORICA_EXPECT_FORTIFY=$<IF:$<CONFIG:Release,RelWithDebInfo,MinSizeRel>,${_di_lorica_fortify},0>"
    DI_LORICA_EXPECT_UNSAFE_API=${_di_lorica_unsafe})
  di_target_warnings(${target})
endfunction()

_di_lorica_applied_check(di_lorica_applied_baseline 0)
di_harden(di_lorica_applied_baseline)
add_test(NAME lorica_applied.baseline COMMAND di_lorica_applied_baseline)

if(DI_LORICA_SERVER_PROFILE STREQUAL "extended")
  _di_lorica_applied_check(di_lorica_applied_server 1)
  di_harden(di_lorica_applied_server SERVER)
  add_test(NAME lorica_applied.server COMMAND di_lorica_applied_server)
else()
  # サーバが baseline に下がっている: 同じ SERVER の経路で作り、baseline の印を確かめたうえで skip にする (通過にしない)。
  _di_lorica_applied_check(di_lorica_applied_server 0)
  di_harden(di_lorica_applied_server SERVER)
  add_test(NAME lorica_applied.server COMMAND di_lorica_applied_server --skip-baseline "${DI_LORICA_SERVER_BASELINE_REASON}")
endif()
set_tests_properties(lorica_applied.baseline lorica_applied.server PROPERTIES
  SKIP_RETURN_CODE 77 TIMEOUT 60 ENVIRONMENT "DI_TEST_FIXTURES=${CMAKE_CURRENT_SOURCE_DIR}/../fixtures")

# compile_commands.json を見るテスト。SERVER で作るビルド単位はここに列挙する (期待値なので di_harden の呼び出しから
# 逆算しない: SERVER を外すと、このテストが EXTENDED のフラグが無いと言って落ちる)。
set(_di_lorica_server_targets di_sqlite di_base di_infra di_app discutere-server di_lorica_applied_server)
get_property(_di_lorica_unsafe_header GLOBAL PROPERTY LORICA_UNSAFE_API_HEADER)
set(_di_lorica_cc_skip "")
if(NOT CMAKE_EXPORT_COMPILE_COMMANDS)
  set(_di_lorica_cc_skip "CMAKE_EXPORT_COMPILE_COMMANDS が OFF (Discutere が top-level でない)")
elseif(NOT CMAKE_GENERATOR MATCHES "Ninja|Makefiles")
  set(_di_lorica_cc_skip "generator ${CMAKE_GENERATOR} は compile_commands.json を作らない")
endif()
get_property(_di_lorica_multi GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(_di_lorica_multi)
  set(_di_lorica_cc_config "")
else()
  set(_di_lorica_cc_config "${CMAKE_BUILD_TYPE}")
endif()
if(MSVC)
  set(_di_lorica_cc_toolchain msvc)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang)$")
  set(_di_lorica_cc_toolchain linux)
else()
  # Lorica の lorica_harden_target はここで configure を止めるので、通常は来ない。来たら skip と明示する。
  set(_di_lorica_cc_toolchain unsupported)
endif()
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
  set(_di_lorica_cf_flag -mbranch-protection=standard)
else()
  set(_di_lorica_cf_flag -fcf-protection=full)
endif()
# baseline のフラグ。-fstack-protector-strong は Lorica が無条件に付ける。残りは Lorica が probe して、対応しない
# コンパイラでは WARNING を出して省く: 省いたもの (probe が偽) は期待しない。EXTENDED の -ftrivial-auto-var-init=zero は
# スクリプトの側で SERVER のビルド単位にだけ求める (EXTENDED は対応しなければ configure が止まる)。
set(_di_lorica_linux_flags "C:-fstack-protector-strong" "CXX:-fstack-protector-strong")
foreach(lang C CXX)
  foreach(flag -fstack-clash-protection ${_di_lorica_cf_flag})
    string(MAKE_C_IDENTIFIER "${flag}" key)
    if(NOT DEFINED _LORICA_PROBE_${lang}_${key} OR _LORICA_PROBE_${lang}_${key})
      list(APPEND _di_lorica_linux_flags "${lang}:${flag}")
    endif()
  endforeach()
endforeach()
string(REPLACE ";" "," _di_lorica_server_targets_arg "${_di_lorica_server_targets}")
string(REPLACE ";" "," _di_lorica_linux_flags_arg "${_di_lorica_linux_flags}")
add_test(NAME lorica_applied.compile_commands COMMAND ${CMAKE_COMMAND}
  "-DCOMPILE_COMMANDS=${CMAKE_BINARY_DIR}/compile_commands.json"
  "-DSKIP_REASON=${_di_lorica_cc_skip}"
  "-DTOOLCHAIN=${_di_lorica_cc_toolchain}"
  "-DCONFIG=${_di_lorica_cc_config}"
  "-DSERVER_PROFILE=${DI_LORICA_SERVER_PROFILE}"
  "-DSERVER_TARGETS=${_di_lorica_server_targets_arg}"
  "-DLINUX_FLAGS=${_di_lorica_linux_flags_arg}"
  "-DFORTIFY=${_di_lorica_fortify}"
  "-DUNSAFE_API=${_di_lorica_unsafe}"
  "-DUNSAFE_API_HEADER=${_di_lorica_unsafe_header}"
  "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
  -P ${CMAKE_CURRENT_SOURCE_DIR}/check_compile_commands.cmake)
set_tests_properties(lorica_applied.compile_commands PROPERTIES
  SKIP_REGULAR_EXPRESSION "LORICA_APPLIED_SKIP:" TIMEOUT 300)

# 実行ファイルの点検 (Lorica の tools/verify_binary.py。標準ライブラリだけで動く)。python3 が無ければ登録しない。
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND AND EXISTS ${DI_LORICA_DIR}/tools/verify_binary.py)
  set(_di_lorica_verify_args "")
  if(CMAKE_READELF)
    set(_di_lorica_verify_args --readelf ${CMAKE_READELF})
  endif()
  add_test(NAME lorica_applied.verify_binary COMMAND ${Python3_EXECUTABLE} -I ${DI_LORICA_DIR}/tools/verify_binary.py
    ${_di_lorica_verify_args} $<TARGET_FILE:discutere-server>)
  set_tests_properties(lorica_applied.verify_binary PROPERTIES TIMEOUT 60)
else()
  message(STATUS "Lorica: python3 (or ${DI_LORICA_DIR}/tools/verify_binary.py) が無いので lorica_applied.verify_binary を登録しない")
endif()
