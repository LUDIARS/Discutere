include_guard(GLOBAL)
# 依存はこのファイルだけで宣言する。版はすべて URL_HASH / commit で固定。
# 取得済みのものを使うときは -DFETCHCONTENT_SOURCE_DIR_<NAME>=<展開先> を渡す (BOOST / JSON / SQLITE / LORICA)。

find_package(OpenSSL 3 REQUIRED)
find_package(Threads REQUIRED)

# nlohmann_json 3.11.3
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(JSON_Install OFF CACHE BOOL "" FORCE)
FetchContent_Declare(json URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d)

# Boost 1.87.0 (Asio / Beast のヘッダだけを使う。SOURCE_SUBDIR に存在しない名前を渡し、ビルドさせない)
FetchContent_Declare(boost URL https://github.com/boostorg/boost/releases/download/boost-1.87.0/boost-1.87.0-b2-nodocs.zip
  URL_HASH SHA256=40efbb1f38b8cf186fd7bb12809f561ae1c233bae0f75146862e9bfce77d0d42
  SOURCE_SUBDIR discutere-header-only)

# SQLite 3.53.4: better-sqlite3 13.0.3 が同梱・コンパイルしている amalgamation と同一物 (移行前と同じ実体)。
# tarball 全体のハッシュで固定する。sqlite3.c 単体の SHA256 は
# 6a2805f8c1ef020a7086e62258519323cf98b219e9fe87a42065d916cfebaefc。
FetchContent_Declare(sqlite URL https://registry.npmjs.org/better-sqlite3/-/better-sqlite3-13.0.3.tgz
  URL_HASH SHA256=77e0513dc1a469fb3bceec4c7fb5ad3f403109787eda05be047ec17fd56868cb
  SOURCE_SUBDIR discutere-no-build)

# Lorica (commit 固定)。ローカルの clone を使うときは -DLORICA_SOURCE_DIR=<path>。
set(LORICA_SOURCE_DIR "" CACHE PATH "Local Lorica checkout (empty = fetch the pinned commit)")
set(LORICA_BUILD_PROTECTION OFF CACHE BOOL "" FORCE)
set(LORICA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
if(LORICA_SOURCE_DIR)
  add_subdirectory(${LORICA_SOURCE_DIR} ${CMAKE_BINARY_DIR}/_lorica EXCLUDE_FROM_ALL)
else()
  FetchContent_Declare(lorica GIT_REPOSITORY https://github.com/LUDIARS/Lorica.git
    GIT_TAG cab4f12086e30484f249d1b4aa479bf7cdec13a9)
  FetchContent_MakeAvailable(lorica)
endif()

FetchContent_MakeAvailable(json boost sqlite)

add_library(di_boost INTERFACE)
target_include_directories(di_boost SYSTEM INTERFACE ${boost_SOURCE_DIR})
target_compile_definitions(di_boost INTERFACE BOOST_ERROR_CODE_HEADER_ONLY BOOST_ASIO_NO_DEPRECATED
  BOOST_BEAST_USE_STD_STRING_VIEW)
if(WIN32)
  target_link_libraries(di_boost INTERFACE ws2_32 mswsock)
endif()

# SQLite は better-sqlite3 の deps/download.sh と同じコンパイル定義で静的ライブラリにする。
add_library(di_sqlite STATIC ${sqlite_SOURCE_DIR}/deps/sqlite3/sqlite3.c)
target_include_directories(di_sqlite SYSTEM PUBLIC ${sqlite_SOURCE_DIR}/deps/sqlite3)
target_compile_definitions(di_sqlite PRIVATE
  HAVE_INT16_T=1 HAVE_INT32_T=1 HAVE_INT8_T=1 HAVE_STDINT_H=1 HAVE_UINT16_T=1 HAVE_UINT32_T=1 HAVE_UINT8_T=1
  SQLITE_DEFAULT_CACHE_SIZE=-16000 SQLITE_DEFAULT_FOREIGN_KEYS=1 SQLITE_DEFAULT_MEMSTATUS=0
  SQLITE_DEFAULT_WAL_SYNCHRONOUS=1 SQLITE_DQS=0 SQLITE_ENABLE_COLUMN_METADATA SQLITE_ENABLE_DBSTAT_VTAB
  SQLITE_ENABLE_DESERIALIZE SQLITE_ENABLE_FTS3 SQLITE_ENABLE_FTS3_PARENTHESIS SQLITE_ENABLE_FTS4
  SQLITE_ENABLE_FTS5 SQLITE_ENABLE_GEOPOLY SQLITE_ENABLE_JSON1 SQLITE_ENABLE_MATH_FUNCTIONS
  SQLITE_ENABLE_PERCENTILE SQLITE_ENABLE_RTREE SQLITE_ENABLE_STAT4 SQLITE_ENABLE_UPDATE_DELETE_LIMIT
  SQLITE_LIKE_DOESNT_MATCH_BLOBS SQLITE_OMIT_DEPRECATED SQLITE_OMIT_PROGRESS_CALLBACK SQLITE_OMIT_SHARED_CACHE
  SQLITE_OMIT_TCL_VARIABLE SQLITE_SOUNDEX SQLITE_THREADSAFE=2 SQLITE_TRACE_SIZE_LIMIT=32 SQLITE_USE_URI=0
  SQLITE_OMIT_LOAD_EXTENSION)
if(MSVC)
  target_compile_options(di_sqlite PRIVATE /w)
else()
  target_compile_options(di_sqlite PRIVATE -w)
  target_link_libraries(di_sqlite PUBLIC ${CMAKE_DL_LIBS} m)
endif()
lorica_harden_target(di_sqlite)
