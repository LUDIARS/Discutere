# job cpp-lorica-20261111 (W4 Discutere) 引き継ぎの状態

ブランチ `cpp/discutere-lorica` (基点 origin/main `3fa595d`、`b414981` で origin/main `f13db8b` を merge 済み)。未 push。
WIP コミットは無い。2 代目の担当が引き継いで、sanitizer・fuzz・層の宣言まで仕上げた。

コミット:

| SHA | 代 | 内容 |
|---|---|---|
| `4a0293e` | 1 | 棚卸し (`inventory.md`) |
| `4442240` | 1 | Lorica を 5e8281b に固定 (gitlink + `DI_LORICA_COMMIT` 照合)、`cmake/DiHardening.cmake` の `di_harden()` で全ビルド単位に適用、`DI_FUZZ` option |
| `919f38e` | 1 | 入力経路の上限 (ws / SQLite / S3 / claude -p) と境界テスト |
| `0509106` | 1 | fuzz の入口 5 本と回帰用の ctest |
| `ed55c3a` | 1 | 引き継ぎ文書 (この文書の初版) |
| `b414981` | 2 | origin/main (#205: Lorica を 5e8281b に上げる) の merge。README・INVENTORY の Lorica の行の衝突はこのブランチ側 (main の内容を含む) を採った |
| `491b546` | 2 | `spec/domains/service-runtime` に `native/cmake/` を登録 |
| `94bace8` | 2 | `.anatomia/layers.json` を新設 (Revisor の未分類ゲートが読む層の宣言、要確認) |
| `fdd0f58` | 2 | TSan の報告を直す (DB singleton のロック順序、S3 の upload 枠) |
| `aac0259` | 2 | 層の宣言に `native/src/db`・`native/src/flow/db` を足す |
| (この文書と同じコミット群) | 2 | fuzz で見つけた front_matter の入口の誤り、層の宣言に `native/src/lib/front_matter`、この文書 |

## (a) 済んで確かめたこと (2 代目、GCC 13.3 / Clang 18.1.3、Linux x86_64、Lorica は submodule を取得して 5e8281b)

- submodule `lib/lorica` を `git submodule update --init lib/lorica` で取得して configure (LORICA_SOURCE_DIR なし)。
  `DI_LORICA_COMMIT` の照合が通り、`Lorica: server targets use the EXTENDED profile`。
- GCC Release: 852/852、警告 0 (`-Werror`)。ctest **209/209 通過** (最終の作業ツリーで再実行)。
- ASan + UBSan (GCC、Debug `-O1 -g1`): 209/209 通過、sanitizer の報告 0。
- TSan (GCC、Debug `-O1 -g1`): 最初は 4 本が報告で落ちた → `fdd0f58` で直して **209/209 通過、報告 0**。
  - `get_flow_db` / `get_main_db`: singleton の mutex を持ったまま DB を開いていた (singleton → DB) のと、
    DB の transaction の中から呼ぶ経路 (DB → singleton) で lock-order-inversion。開く処理を別の mutex に分けた。
  - `upload_file_to_s3`: `std::timed_mutex::try_lock_for` を GCC 13 の TSan が捕まえず「ロックしていない mutex の解放」と
    誤検知。mutex + condition_variable の枠に置き換えた (振る舞いは同じ、回帰テストを足した)。
- Clang `-DDI_FUZZ=ON -DDI_SANITIZE=address,undefined`: 5 本を各 30 秒。json・gateway_frame・claude_cli_result・s3_xml は
  落ちず。front_matter は入口の誤り (`stringify` が仕様どおり投げる `std::runtime_error` を入口が受けていなかった) で
  落ちた → 入口を直し、再現の入力を `tests/fuzz/corpus/front_matter/content_lang.md` に足した (GCC の `fuzz.front_matter` が
  直す前の入口で落ちることを確かめた)。
- `verify_binary.py` を Release の実行ファイル 210 本 (server 1 + テスト 204 + 回帰 5) すべてに掛けて終了コード 0。
- `compile_commands.json` (637 件): 守りの設定がすべてにあり、打ち消す設定は 0 件。
- `anatomia domains program --repo . --unclassified`: origin/main からこのブランチで変えたファイルに未分類は無い。

## (b) 済んだが確かめていないこと

- MSVC / Windows のビルド、Clang の通常ビルド、本社での結合テスト・実機 (外部サービスへの実接続)。

## (c) 途中のこと

無し。

## (d) 未着手のこと

- Slack の payload 文字列の入れ子 JSON を同じ budget で数える改善、Gateway の WS 層と JSON 層の budget を 1 依頼 1 budget に
  寄せるか (どれも上限値の内側)。
- `.anatomia/layers.json` で、この job で変えていない native/src (約 70 module) と Node 版の層は宣言していない。
- 「未適用のビルド単位が残ったら configure を止める」仕掛け (今は全ビルド単位が `di_harden()` を呼ぶことを CMake で確かめただけ)。

## (e) 分かっている失敗

無し。

## (f) 人間の判断が要ること

1. Lorica の取り込み方: FetchContent ではなく main の 4833ab3 で選んだ submodule (`lib/lorica`) を保った。
2. SQLite amalgamation の取得元が npm レジストリ (SHA256 固定)。
3. `DI_LORICA_PROFILE=auto` は EXTENDED が使えない環境で baseline に下がる。本社で必ず EXTENDED にするか。
4. `.anatomia/layers.json` の層の割り当て (要確認): `native/src/{backup,base,db,flow/db,infra,persona_engine/llm}` = infrastructure、
   `native/src/lib/front_matter` = domain、`native/tests/**` = verification。`base` は純関数も、`infra` は HTTP サーバの受け口も持つ。
5. `spec/domains`: `native/cmake/` を service-runtime に入れた。`native/docs/` はどのドメインも文書を持たないので入れていない。
   このブランチで変えた `native/CMakeLists.txt`・`native/README.md`・`native/src/base/sqlite.*`・`native/src/infra/ws_client_impl.hpp`・
   `native/src/persona_engine/llm/claude_cli.*` は main の時点から spec/domains に入っていない。

## (g) Lorica の適用表と上限チェック

固定: `5e8281b0e60fa16f814e16eaa620715937e8618d`。

| ビルド単位 | boundary | harden | unsafe API 拒否 |
|---|---|---|---|
| `discutere-server` (EXE) | あり | EXTENDED (auto で選ばれた) | あり |
| `di_base` `di_infra` `di_app` (STATIC) | あり | EXTENDED | あり |
| `di_sqlite` (C, 第三者 SQLite) | なし (C) | EXTENDED | 不可 (C ソースは Lorica が拒否) |
| `di_test_*` (204 本) | あり | baseline | あり |
| `di_fuzz_regress_*` (5 本) / `di_fuzz_*` (DI_FUZZ 時) | あり | baseline | あり |
| OpenSSL (システム) | — | 対象外 (ビルド済み) | — |
| Boost / nlohmann_json | ヘッダのみ (Beast の実装は di_infra でコンパイル → di_infra の守り) | | |
| Kuzu | C++ ビルドに入っていない (常駐サーバは SQLite だけを開く)。上限チェックの対象外 | | |

上限チェック:

| 経路 | 場所 | 部品 | 上限と根拠 |
|---|---|---|---|
| Discord Gateway / Slack の WS フレーム | `native/src/infra/ws_client_impl.hpp` websocket_handshake (pmd 無効 + `read_message_max`)、on_read (`input_budget`) | input_budget / Beast | Gateway 8MiB・Slack 4MiB (既存の値)。圧縮は交渉しない → RSV1 は断る |
| Gateway の JSON | `native/src/discord_hook/gateway_client.cpp` (parse_json) | input_budget (parse_json 内) | 8MiB / 500,000 要素 / 深さ 64 (既存) |
| SQLite から読む値 | `native/src/base/sqlite.cpp` get / all / each | input_budget | 512MiB・200 万行 (既存の QueryLimits)。get/each も呼び出し側で絞れる |
| S3 バックアップ | `native/src/backup/s3.cpp` s3_part_count / s3_part_range / s3_xml_tag / read_range / ETag | checked_multiply / checked_add / contains_range / input_budget | part 10,000 (S3 の仕様)、ETag 1024 バイト、読み出しはファイルの大きさ・part 数ちょうど |
| `claude -p` の出力 | `native/src/persona_engine/llm/claude_cli.cpp` (max_stdout → `base/process.cpp` の input_budget、parse_json) | input_budget | 16MiB (lib/one_shot の既定のまま)、設定可能に |

## (h) 次にやる順番

1. まとめ役の受け入れテスト (別のビルド) と push / PR。
2. (d) の残り。

## (i) ローカルのビルドとテストの手順

```
cd <repo>
git submodule update --init lib/lorica          # または -DLORICA_SOURCE_DIR=<5e8281b の Lorica checkout>
export CCACHE_DIR=/home/user/.cpp-deps/ccache   # 任意
cmake -S native -B native/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  [-DFETCHCONTENT_SOURCE_DIR_BOOST=/home/user/.cpp-deps/src/boost-1.87.0] \
  [-DFETCHCONTENT_SOURCE_DIR_JSON=/home/user/.cpp-deps/src/nlohmann_json/json] \
  [-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache] -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
/home/user/.cpp-deps/bin/buildslot cmake --build native/build -j2
cd native/build && /home/user/.cpp-deps/bin/buildslot ctest -j2 --output-on-failure
python3 -I <Lorica>/tools/verify_binary.py --readelf /usr/bin/readelf native/build/bin/discutere-server
```

sanitizer: `-DCMAKE_BUILD_TYPE=Debug "-DCMAKE_{C,CXX}_FLAGS_DEBUG=-O1 -g1" -DDI_SANITIZE=address,undefined` (または `thread`)。
テストの実行ファイルは sanitizer 付きで 1 本 30〜40MB あり、204 本を並べると 6GB を超える。ディスクが限られる環境では、
ローカルの configure だけで薄い archive (`"-DCMAKE_{C,CXX}_ARCHIVE_CREATE=<CMAKE_AR> qcT <TARGET> <LINK_FLAGS> <OBJECTS>"`、
APPEND は `qT`) にし、`discutere-server` を先に作ってから (main.main が起動する)、テストを 20 本ずつ
ビルド → `ctest -R` → 実行ファイルを消す、を繰り返すと 2GB 以下で回る。

fuzz: `-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DDI_FUZZ=ON -DDI_SANITIZE=address,undefined` で
`di_fuzz_<name>` を作り、corpus の **写し** を渡す (libFuzzer は最初の corpus のディレクトリに入力を書き足す)。
例 `di_fuzz_json -max_total_time=30 <写し>`。リポへの経路が symlink で大文字小文字が違うと、Clang が強制 include の
`-Wnonportable-include-path` で止まる (`-S` に実際のパスを渡す)。

option: `DI_LORICA_PROFILE` (auto/extended/baseline)、`DI_DEPRECATE_UNSAFE_APIS` (ON)、`DI_LORICA_ALLOW_OTHER_COMMIT` (OFF)、
`DI_SANITIZE`、`DI_FUZZ` (Clang のみ、OFF)。SQLite は configure 時に npm レジストリから取得 (SHA256 固定)。
