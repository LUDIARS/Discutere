cmake_minimum_required(VERSION 3.24)
project(DiscutereNative VERSION 0.1.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
include(FetchContent)
include(CTest)

option(DI_WARNINGS_AS_ERRORS "Treat compiler warnings in Discutere sources as errors" ON)

# 検査用ビルド: -DDI_SANITIZE=address,undefined か -DDI_SANITIZE=thread (GCC / Clang、Debug で使う)。
# 取得した第三者のコードも含め全体に掛ける (片側だけだと誤検知・見逃しが出る)。
set(DI_SANITIZE "" CACHE STRING "Sanitizers for a checking build (e.g. address,undefined or thread)")
if(DI_SANITIZE)
  if(MSVC)
    message(FATAL_ERROR "DI_SANITIZE is supported with GCC / Clang only")
  endif()
  add_compile_options(-fsanitize=${DI_SANITIZE} -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
  add_link_options(-fsanitize=${DI_SANITIZE})
  if(DI_SANITIZE MATCHES "thread" AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # GCC は Boost.Asio が使う atomic_thread_fence を TSan で扱えないと警告する (-Wtsan)。-Werror で止めない。
    add_compile_options(-Wno-tsan)
  endif()
endif()

# fuzz の入口 (libFuzzer、Clang のみ。既定 OFF で GCC のビルドには影響しない)。ON のときは全体を
# -fsanitize=fuzzer-no-link で計装する (入口以外のコードの到達も libFuzzer に見えるように)。
option(DI_FUZZ "Build libFuzzer entry points (Clang only)" OFF)
if(DI_FUZZ)
  if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR "DI_FUZZ needs Clang (libFuzzer)")
  endif()
  add_compile_options(-fsanitize=fuzzer-no-link)
endif()

include(cmake/DiDependencies.cmake)
include(cmake/DiWarnings.cmake)
include(cmake/DiHardening.cmake)
# 第三者の SQLite amalgamation (ソースからビルドする C)。守りは掛け、unsafe API の拒否は C なので掛けない。
di_harden(di_sqlite SERVER C_SOURCES)

# base: 文字列・JSON・暗号・ログ・子プロセス・SQLite ラッパ (Boost 非依存)。
file(GLOB_RECURSE DI_BASE_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/base/*.cpp)
# 画面の埋め込み (native/web/**)。
file(GLOB_RECURSE DI_WEB_FILES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/web/*)
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/web_assets.cpp
  COMMAND ${CMAKE_COMMAND} -DWEB_DIR=${CMAKE_CURRENT_SOURCE_DIR}/web -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/web_assets.cpp
    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedWeb.cmake
  DEPENDS ${DI_WEB_FILES} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedWeb.cmake VERBATIM)
add_library(di_base STATIC ${DI_BASE_SOURCES} ${CMAKE_CURRENT_BINARY_DIR}/web_assets.cpp)
target_include_directories(di_base PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(di_base PUBLIC Lorica::boundary nlohmann_json::nlohmann_json di_sqlite
  OpenSSL::SSL OpenSSL::Crypto Threads::Threads)
di_target_warnings(di_base)
di_harden(di_base SERVER)

# infra: Boost.Beast を含む翻訳単位はここだけに閉じ込める (重いので)。
file(GLOB_RECURSE DI_INFRA_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/infra/*.cpp)
add_library(di_infra STATIC ${DI_INFRA_SOURCES} ${CMAKE_BINARY_DIR}/generated/beast_src.cpp)
target_link_libraries(di_infra PUBLIC di_base PRIVATE di_boost)
if(WIN32)
  target_link_libraries(di_infra PRIVATE crypt32)
endif()
di_target_warnings(di_infra)
di_harden(di_infra SERVER)

# app: TS の src/ を写したモジュール群 (base / infra / main.cpp 以外の全 .cpp)。
file(GLOB_RECURSE DI_APP_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
list(FILTER DI_APP_SOURCES EXCLUDE REGEX "/src/(base|infra)/")
list(FILTER DI_APP_SOURCES EXCLUDE REGEX "/src/main\\.cpp$")
if(DI_APP_SOURCES)
  add_library(di_app STATIC ${DI_APP_SOURCES})
else()
  add_library(di_app STATIC ${CMAKE_CURRENT_SOURCE_DIR}/cmake/empty.cpp)
endif()
target_link_libraries(di_app PUBLIC di_infra)
di_target_warnings(di_app)
di_harden(di_app SERVER)

add_executable(discutere-server src/main.cpp)
target_link_libraries(discutere-server PRIVATE di_app)
di_target_warnings(discutere-server)
di_harden(discutere-server SERVER)
set_target_properties(discutere-server PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
  RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin" RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin")
install(TARGETS discutere-server RUNTIME DESTINATION bin)

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
