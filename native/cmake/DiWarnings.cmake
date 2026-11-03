include_guard(GLOBAL)
# Discutere のソースだけに警告を課す (取得した第三者のコードには課さない)。
function(di_target_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /utf-8 /W4 /permissive- /bigobj)
    target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
    if(DI_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
      # GCC 13 の -O2/-O3 は std::variant / std::optional の move をインライン展開した後に
      # -Wmaybe-uninitialized の誤検知を出す (GCC PR 80635 系。sql::Value や optional<vector> で多数)。
      # この診断だけを GCC で外す。未初期化の読みは Debug の -Wall / Clang / MSVC (/W4) と ASan で見る。
      target_compile_options(${target} PRIVATE -Wno-maybe-uninitialized)
    endif()
    if(DI_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()
