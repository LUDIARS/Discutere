include_guard(GLOBAL)
# Lorica の守りをビルド単位に掛ける (job cpp-lorica-20261111)。すべてのビルド単位はここの関数を通す。
#
# - di_harden(<t>): Lorica::boundary をリンクし、lorica_harden_target(<t>) (baseline) を掛ける。
#   DI_DEPRECATE_UNSAFE_APIS=ON (既定) なら lorica_deprecate_unsafe_apis(<t>) も掛ける (C++ のビルド単位だけ)。
# - di_harden(<t> SERVER): 常駐サーバに入るビルド単位。DI_LORICA_PROFILE に従い EXTENDED を足す。
#
# DI_LORICA_PROFILE:
#   auto (既定) … このツールチェインで EXTENDED が使えるなら EXTENDED、使えなければ baseline (理由を STATUS で出す)
#   extended   … EXTENDED を必須にする (使えなければ configure を止める。Lorica の挙動)
#   baseline   … baseline だけ
# baseline 自体が使えない環境では lorica_harden_target が configure を止める (守りを外して通さない)。
set(DI_LORICA_PROFILE "auto" CACHE STRING "Lorica hardening profile for the server targets: auto, extended or baseline")
set_property(CACHE DI_LORICA_PROFILE PROPERTY STRINGS auto extended baseline)
option(DI_DEPRECATE_UNSAFE_APIS "Reject strcpy/strcat/sprintf/vsprintf/wcscpy/wcscat in Discutere C++ targets" ON)

if(NOT DI_LORICA_PROFILE MATCHES "^(auto|extended|baseline)$")
  message(FATAL_ERROR "DI_LORICA_PROFILE は auto / extended / baseline のどれか (今は '${DI_LORICA_PROFILE}')")
endif()

set(_di_server_extended OFF)
if(DI_LORICA_PROFILE STREQUAL "extended")
  set(_di_server_extended ON)
elseif(DI_LORICA_PROFILE STREQUAL "auto")
  lorica_extended_hardening_supported(_di_extended_ok)
  if(_di_extended_ok)
    set(_di_server_extended ON)
  else()
    message(STATUS "Lorica EXTENDED はこの環境で使えないので、サーバは baseline: ${_di_extended_ok_REASON}")
  endif()
endif()
# 選んだ結果は tests/lorica_applied (ビルドの結果を見るテスト) が期待値として読む。
if(_di_server_extended)
  message(STATUS "Lorica: server targets use the EXTENDED profile")
  set(DI_LORICA_SERVER_PROFILE extended)
  set(DI_LORICA_SERVER_BASELINE_REASON "")
else()
  message(STATUS "Lorica: server targets use the baseline profile")
  set(DI_LORICA_SERVER_PROFILE baseline)
  if(DI_LORICA_PROFILE STREQUAL "baseline")
    set(DI_LORICA_SERVER_BASELINE_REASON "DI_LORICA_PROFILE=baseline")
  else()
    set(DI_LORICA_SERVER_BASELINE_REASON "DI_LORICA_PROFILE=auto: ${_di_extended_ok_REASON}")
  endif()
endif()

set(_di_unsafe_api_ok OFF)
if(DI_DEPRECATE_UNSAFE_APIS)
  lorica_unsafe_api_rejection_supported(_di_unsafe_api_ok)
  if(NOT _di_unsafe_api_ok)
    message(STATUS "Lorica: unsafe API rejection is not available here: ${_di_unsafe_api_ok_REASON}")
    set(DI_LORICA_UNSAFE_API_SKIP_REASON "${_di_unsafe_api_ok_REASON}")
  endif()
else()
  set(DI_LORICA_UNSAFE_API_SKIP_REASON "DI_DEPRECATE_UNSAFE_APIS=OFF")
endif()
set(DI_LORICA_UNSAFE_API_REJECTION ${_di_unsafe_api_ok})

function(di_harden target)
  cmake_parse_arguments(PARSE_ARGV 1 arg "SERVER;C_SOURCES" "" "")
  # C だけのビルド単位 (SQLite) は C++ のヘッダ部品を使わないのでリンクしない。
  if(NOT arg_C_SOURCES)
    target_link_libraries(${target} PRIVATE Lorica::boundary)
  endif()
  if(arg_SERVER AND _di_server_extended)
    lorica_harden_target(${target} EXTENDED)
  else()
    lorica_harden_target(${target})
  endif()
  # C のソースを持つビルド単位 (第三者の SQLite) は Lorica が拒否する (C++ のヘッダしか差し込めない)。
  if(_di_unsafe_api_ok AND NOT arg_C_SOURCES)
    lorica_deprecate_unsafe_apis(${target})
  endif()
endfunction()
