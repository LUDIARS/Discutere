# Discutere native (C++)

`src/` の Node.js 常駐サーバ (`src/index.ts`) を C++20 に移したもの (job `cpp-node-20261030`)。
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
| Lorica | commit cab4f12 | GIT_TAG |
| OpenSSL | 3.x | システム |

Node のまま残す機能 (PDF/DOCX 抽出・Notion クロール・tar.gz 作成) は `sidecar/` を別プロセスで呼ぶ (`sidecar/README.md`)。

## ビルドとテスト

```
cmake -S native -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
cd build && ctest --output-on-failure -j2
```

取得済みの依存を使うときは `-DFETCHCONTENT_SOURCE_DIR_{BOOST,JSON,SQLITE}=<展開先>` と `-DLORICA_SOURCE_DIR=<clone>`
(`tools/build.sh` が既定の場所を見る)。実行ファイルは `build/bin/discutere-server`。

## 起動

作業ディレクトリをリポのルートにし、移植前と同じ env (`BACKEND_PORT`・`DATABASE_PATH` 等) と `discutere.config.json` で起動する。
サイドカーを使うので、リポのルートに `npm install` 済みの `node_modules` (pdf-parse / mammoth / tar / @ludiars/canalis / tsx) が要る。

```
BACKEND_PORT=3110 ./native/build/bin/discutere-server
```

## 互換性の検査 (本社で本番相当のデータ)

`docs/port-notes/w6-main.md` §6。Node 版と C++ 版を同じデータで並べて起動し、副作用の無い GET を比べる:

```
npx tsx native/tools/compat-check.ts --cpp-bin build/bin/discutere-server \
  --data <data のスナップショット> --config <config のコピー> --out /tmp/compat-report.md
```
