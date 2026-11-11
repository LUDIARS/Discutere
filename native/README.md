# Discutere native (C++)

Discutere の常駐サーバ (C++20)。Node.js 版 (`src/index.ts`) から移したもの (job `cpp-node-20261030`)。
目的は脆弱性の受け口と余分な依存を減らすこと。HTTP の経路・JSON の形・ポート・DB スキーマは移植前と同じ。

- 棚卸し・依存・安全の対応表: `docs/INVENTORY.md`
- 書き方の規約: `docs/PORTING.md`
- 担当ごとの対応表・テスト・挙動の差: `docs/port-notes/*.md` (w6-main.md にサーバの組み立てと互換性検査)

## 依存

| 依存 | 版 | 固定 |
|---|---|---|
| Boost (Asio / Beast、ヘッダのみ) | 1.87.0 | URL_HASH SHA256 |
| nlohmann_json | 3.11.3 | URL_HASH SHA256 |
| SQLite (better-sqlite3 13.0.3 同梱の amalgamation) | 3.53.4 | URL_HASH SHA256 (npm tarball) |
| Lorica | commit 5e8281b (`5e8281b0e60fa16f814e16eaa620715937e8618d`) | submodule `lib/lorica` (gitlink)。`DiDependencies.cmake` の `DI_LORICA_COMMIT` と照合 |
| OpenSSL | 3.x | システム |

Node のまま残す機能 (PDF/DOCX 抽出・Notion クロール・tar.gz 作成) は `sidecar/` を別プロセスで呼ぶ (`sidecar/README.md`)。

## ビルドとテスト

```
cmake -S native -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
cd build && ctest --output-on-failure -j2
```

Lorica は submodule なので、最初に `git submodule update --init lib/lorica` を実行する (別の checkout を使うなら
`-DLORICA_SOURCE_DIR=<path>`)。取得済みの Boost / JSON / SQLite を使うときは
`-DFETCHCONTENT_SOURCE_DIR_{BOOST,JSON,SQLITE}=<展開先>` (`tools/build.sh` が既定の場所を見る)。
実行ファイルは `build/bin/discutere-server`。

## Lorica の守り (job cpp-lorica-20261111)

全ビルド単位は `cmake/DiHardening.cmake` の `di_harden()` を通す: `Lorica::boundary` のリンク、`lorica_harden_target`、
C++ のビルド単位には `lorica_deprecate_unsafe_apis` (`strcpy` 等の使用をコンパイルエラーにする。`DI_DEPRECATE_UNSAFE_APIS`)。
常駐サーバに入る単位 (`di_sqlite` `di_base` `di_infra` `di_app` `discutere-server`) は `DI_LORICA_PROFILE` に従う:
`auto` (既定。使えれば EXTENDED、使えなければ baseline で理由を出す) / `extended` (必須) / `baseline`。
Lorica の checkout が git なら HEAD を `DI_LORICA_COMMIT` と照合し、違えば configure を止める
(試すときだけ `-DDI_LORICA_ALLOW_OTHER_COMMIT=ON`)。できた実行ファイルは Lorica の
`tools/verify_binary.py build/bin/discutere-server` で点検する。

## fuzz の入口

`tests/fuzz/fuzz_<name>.cpp` (libFuzzer 形式)。いつものビルドでは固定の入力で 1 回ずつ呼ぶ `fuzz.<name>` が ctest に入る。
libFuzzer で回すときは Clang で `-DDI_FUZZ=ON` (`tests/fuzz/CMakeLists.txt` の例)。

## 起動

作業ディレクトリをリポのルートにし、移植前と同じ env (`BACKEND_PORT`・`DATABASE_PATH` 等) と `discutere.config.json` で起動する。
サイドカーを使うので、リポのルートに `npm install` 済みの `node_modules` (pdf-parse / mammoth / tar / @ludiars/canalis / tsx) が要る。

```
BACKEND_PORT=3110 ./native/build/bin/discutere-server
```

## 検査用ビルド

```
cmake -S native -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug "-DCMAKE_CXX_FLAGS_DEBUG=-O1 -g1" \
  "-DCMAKE_C_FLAGS_DEBUG=-O1 -g1" -DDI_SANITIZE=address,undefined
cmake -S native -B build-tsan -G Ninja -DCMAKE_BUILD_TYPE=Debug "-DCMAKE_CXX_FLAGS_DEBUG=-O1 -g1" \
  "-DCMAKE_C_FLAGS_DEBUG=-O1 -g1" -DDI_SANITIZE=thread
```

デバッグ情報を `-g1` に絞る (既定の `-g` だとテスト 202 本で 30GB 近くになる)。

## Node 版について

移行元の Node 版サーバ (`src/index.ts` とサーバ専用のモジュール) は撤去した。`src/` に残る TS は運用スクリプト
(`scripts/`) とサイドカー (`sidecar/`) が使う分だけ。Node 版と出力を比べる `compat-check` と、Node の入出力から
期待値を作る golden 生成スクリプトも撤去した。`tests/fixtures/` の期待値は撤去時点の値で固定している。
