cmake_minimum_required(VERSION 3.24)
project(DiscutereNative VERSION 0.1.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
include(FetchContent)
include(CTest)

option(DI_WARNINGS_AS_ERRORS "Treat compiler warnings in Discutere sources as errors" ON)

include(cmake/DiDependencies.cmake)
include(cmake/DiWarnings.cmake)

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
lorica_harden_target(di_base)

# infra: Boost.Beast を含む翻訳単位はここだけに閉じ込める (重いので)。
file(GLOB_RECURSE DI_INFRA_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/infra/*.cpp)
add_library(di_infra STATIC ${DI_INFRA_SOURCES})
target_link_libraries(di_infra PUBLIC di_base PRIVATE di_boost)
if(WIN32)
  target_link_libraries(di_infra PRIVATE crypt32)
endif()
di_target_warnings(di_infra)
lorica_harden_target(di_infra)

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
lorica_harden_target(di_app)

add_executable(discutere-server src/main.cpp)
target_link_libraries(discutere-server PRIVATE di_app)
di_target_warnings(discutere-server)
lorica_harden_target(discutere-server)
set_target_properties(discutere-server PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
  RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin" RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin")
install(TARGETS discutere-server RUNTIME DESTINATION bin)

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
