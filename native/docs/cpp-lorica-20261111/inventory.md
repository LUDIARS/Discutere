# Discutere 棚卸し (job cpp-lorica-20261111, W4)

基点: `cpp/discutere-lorica` = origin/main `3fa595d`。作成 2026-11-11。返事を待たずに先へ進む。

## 1. ビルド単位 (native/、main の時点)

| ビルド単位 | 種類 | 中身 | Lorica::boundary | lorica_harden_target | deprecate_unsafe_apis |
|---|---|---|---|---|---|
| `di_base` | STATIC | `src/base/**` + 生成 `web_assets.cpp` | あり (PUBLIC) | baseline | なし |
| `di_infra` | STATIC | `src/infra/**` + 生成 `beast_src.cpp` (Boost.Beast の実装 1 単位) | あり (di_base 経由) | baseline | なし |
| `di_app` | STATIC | `src/**` (base/infra/main 以外、約 400 .cpp) | あり (di_base 経由) | baseline | なし |
| `discutere-server` | EXE (常駐) | `src/main.cpp` | あり (経由) | baseline | なし |
| `di_sqlite` | STATIC (C, 第三者) | SQLite 3.53.4 amalgamation | なし (C) | baseline (main で既に適用) | 不可 (C ソース。Lorica が拒否) |
| `di_test_*` (202 本) | EXE | `tests/**/*_tests.cpp` | あり (経由) | baseline | なし |
| fuzz の入口 | — | **無い** | — | — | — |

- Lorica 自身: `lorica_boundary` は INTERFACE (コンパイルしない)。`LORICA_BUILD_PROTECTION OFF` (sealed_value は不使用)。
- Boost / nlohmann_json はヘッダのみ (Beast の実装は di_infra の 1 単位でコンパイル → di_infra の守りが掛かる)。
- OpenSSL はシステムのビルド済み → 対象外 (ビルド済み)。
- Kuzu: C++ ビルドに入っていない。常駐サーバは Kuzu グラフを開かない (`KuzuClient` は名前に反して SQLite)。npm `kuzu` は CLI スクリプト (Node) だけが使う。

## 2. Lorica の固定

- main: submodule `lib/lorica` の gitlink = `cab4f12086e30484f249d1b4aa479bf7cdec13a9`。`DiDependencies.cmake` は FetchContent ではなく
  submodule を `add_subdirectory` する (`-DLORICA_SOURCE_DIR` で上書き可)。main の 4833ab3 で FetchContent → submodule に意図して移した。
- 今回: gitlink を `5e8281b0e60fa16f814e16eaa620715937e8618d` に上げ、CMake に期待 commit を書いて照合する (submodule 方式は保つ。
  指示書 3.1 の「2 つの形」と違う点は人間の判断へ)。
- cab4f12 → 5e8281b の差は追加のみ (EXTENDED / unsafe API / C++20 checked API)。ローカル `/home/user/Lorica` (5e8281b) で configure 通過を確認済み。

## 3. 外から入る入力の経路 (W4 の対象)

| 経路 | 入口 | 今の上限 (main) | 不足 |
|---|---|---|---|
| Discord Gateway WS フレーム | `infra/ws_client_impl.hpp` on_read / `discord_hook/gateway_client.cpp` | Beast `read_message_max` 8MiB + input_budget、JSON は parse_json (8MiB / 500k 要素 / 深さ 64) | 圧縮: Gateway は `encoding=json` で圧縮なし、permessage-deflate も未交渉 (Beast 既定)。圧縮フレーム (RSV1) を拒否することのテストが無い。ws 層単体の上限ちょうど/+1 テストが無い |
| Discord REST 応答 | `discord_hook/discord_api.cpp` | max_response_bytes 8MiB + parse_json | なし |
| Slack Socket Mode / Web API | `slack/socket_mode.cpp` `slack/web_api.cpp` | WS 4MiB、parse_json、Web API 1MiB | payload 文字列の入れ子 JSON を別 budget で読む (報告に記載) |
| SQLite から読む値 | `base/sqlite.cpp` all/get/each | all: 行数 2M / 512MiB、get/each: 1 行 512MiB | get/each の上限を呼び出し側で絞れない・境界テストが all だけ |
| Kuzu | — | 常駐サーバでは開かない (SQLite のみ) | なし |
| S3 バックアップ | `backup/s3.cpp` | 応答 1MiB、part 数 10,000、ファイル読み (fs::read_file) | part 数・offset の計算が生の `+` `*`、part の読み出し (`read_range`) に budget 無し、XML 抜き出しの位置計算が生の `+` |
| `claude -p` の出力 | `lib/one_shot` → `base/process.cpp` / `persona_engine/llm/claude_cli.cpp` | stdout 16MiB (input_budget)、parse_json 16MiB | 上限を設定できず境界テストが無い (16MiB を流す必要) |

(W4 対象外だが既にあるもの: HTTP サーバ本文 (ルート別 body_limit + budget)、外部 HTTP 応答、設定ファイル、front matter、kg_sync import、サイドカー出力、仕様書抽出。)

## 4. 依存 (前回の作業ブランチ = main が既に使っているもの)

| 依存 | 版・固定 | 指示書の許可 |
|---|---|---|
| Boost 1.87.0 (ヘッダ) | URL_HASH SHA256 40efbb1f… | 許可 |
| nlohmann_json 3.11.3 | URL_HASH SHA256 d6c65aca… | 許可 |
| OpenSSL 3 (システム) | find_package | 許可 |
| SQLite 3.53.4 amalgamation | **npm レジストリ** `better-sqlite3-13.0.3.tgz` URL_HASH SHA256 77e0513d… | 許可 (取得元が npm である点は人間の判断へ) |
| Lorica | submodule gitlink | 必須 |
| Threads / dl / m | システム | OS API |

許可外の依存は無い。npm 側 (package.json) は Node の運用スクリプト・サイドカー用に残っている (直接 16 / prod 推移 161)。

## 5. 前回の報告の残り

- `native/docs/INVENTORY.md` / `PORTING.md` / `port-notes/` (21 本)。fuzz の入口が無い。EXTENDED は未適用。unsafe API の拒否は未適用。
- 進行中の計画 `spec/plan/2026-11-10-native-event-loop-async.md` (非同期化) はこの job の範囲外として触らない。
- Excubitor catalog は既に C++ の実行ファイルを指している (catalog の変更は不要)。
