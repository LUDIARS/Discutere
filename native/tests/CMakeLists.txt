# tests/**/<name>_tests.cpp を 1 本ずつ実行ファイルにして ctest に登録する。
# テスト名は <ディレクトリ>.<name> (例 base.json)。
file(GLOB_RECURSE DI_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*_tests.cpp)
foreach(source ${DI_TEST_SOURCES})
  file(RELATIVE_PATH rel ${CMAKE_CURRENT_SOURCE_DIR} ${source})
  get_filename_component(dir ${rel} DIRECTORY)
  get_filename_component(stem ${rel} NAME_WE)
  string(REGEX REPLACE "_tests$" "" stem ${stem})
  string(REPLACE "/" "." group "${dir}")
  if(group STREQUAL "")
    set(test_name ${stem})
  else()
    set(test_name ${group}.${stem})
  endif()
  string(REPLACE "." "_" target_name "di_test_${test_name}")
  add_executable(${target_name} ${source})
  target_include_directories(${target_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  # テストは Beast で偽のサーバ (WebSocket 等) を立てることがあるので di_boost も使える。
  target_link_libraries(${target_name} PRIVATE di_app di_boost)
  di_target_warnings(${target_name})
  lorica_harden_target(${target_name})
  add_test(NAME ${test_name} COMMAND ${target_name})
  set_tests_properties(${test_name} PROPERTIES
    ENVIRONMENT "DI_TEST_FIXTURES=${CMAKE_CURRENT_SOURCE_DIR}/fixtures;DI_REPO_ROOT=${PROJECT_SOURCE_DIR}/.."
    TIMEOUT 120)
endforeach()
