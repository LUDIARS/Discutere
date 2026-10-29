#!/usr/bin/env bash
# 同時に走るフルビルドを 2 本までに絞ってビルドする (4 CPU / 15GB の作業環境向け)。
# 使い方: native/tools/build.sh <build-dir> [ctest 引数...]
#   DI_DEPS_DIR (既定 /home/user/deps-cache/src) に取得済みの boost-1.87.0 / json / sqlite を置くと再取得しない。
#   LORICA_SOURCE_DIR (既定 /home/user/lorica) をローカルの Lorica clone に向ける。
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
build="${1:?build dir}"; shift || true
deps="${DI_DEPS_DIR:-/home/user/deps-cache/src}"
lorica="${LORICA_SOURCE_DIR:-/home/user/lorica}"
slots="${DI_BUILD_SLOTS_DIR:-/tmp/di-build-slots}"
mkdir -p "$slots"
extra=()
[ -d "$deps/boost-1.87.0" ] && extra+=("-DFETCHCONTENT_SOURCE_DIR_BOOST=$deps/boost-1.87.0")
[ -d "$deps/json" ] && extra+=("-DFETCHCONTENT_SOURCE_DIR_JSON=$deps/json")
[ -d "$deps/sqlite/package" ] && extra+=("-DFETCHCONTENT_SOURCE_DIR_SQLITE=$deps/sqlite/package")
[ -d "$lorica" ] && extra+=("-DLORICA_SOURCE_DIR=$lorica")
run_build() {
  if [ ! -f "$build/CMakeCache.txt" ]; then
    cmake -S "$here" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Debug "${extra[@]}"
  fi
  cmake --build "$build" -j "${DI_BUILD_JOBS:-2}"
}
# 2 つのスロットのどちらかを取るまで待つ。
while true; do
  for s in 1 2; do
    exec {fd}>"$slots/slot$s.lock"
    if flock -n "$fd"; then
      run_build
      flock -u "$fd"
      if [ "$#" -gt 0 ] || [ "${DI_RUN_TESTS:-0}" = 1 ]; then
        (cd "$build" && ctest --output-on-failure "$@")
      fi
      exit 0
    fi
    exec {fd}>&-
  done
  sleep 5
done
