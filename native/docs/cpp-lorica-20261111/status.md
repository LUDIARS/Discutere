# job cpp-lorica-20261111 (W4 Discutere) 引き継ぎの状態

ブランチ `cpp/discutere-lorica` (基点 origin/main `3fa595d`)。未 push。WIP コミットは無い (途中の変更は残っていない)。

コミット:

| SHA | 内容 |
|---|---|
| `4a0293e` | 棚卸し (`inventory.md`) |
| `4442240` | Lorica を 5e8281b に固定 (gitlink + `DI_LORICA_COMMIT` 照合)、`cmake/DiHardening.cmake` の `di_harden()` で全ビルド単位に適用、`DI_FUZZ` option |
| `919f38e` | 入力経路の上限 (ws / SQLite / S3 / claude -p) と境界テスト |
| `0509106` | fuzz の入口 5 本と回帰用の ctest |
| (この文書のコミット) | 引き継ぎ文書 |

## (a) 済んで確かめたこと

いずれもクラウドの作業環境 (GCC 13.3, Linux x86_64, Lorica は `-DLORICA_SOURCE_DIR=/home/user/Lorica` = 5e8281b) で、
`0509106` と同じ作業ツリー (最終状態) を 1 回ビルドして確かめた。**途中のコミット (4442240, 919f38e) を単独ではビルドしていない。**

- configure (Release, Ninja): 通過。`Lorica: server targets use the EXTENDED profile` (GCC 13 で EXTENDED の要件を満たす)。
  unsafe API の拒否も使える判定。`DI_LORICA_COMMIT` の照合は /home/user/Lorica (git, HEAD 5e8281b) で通過。
- ビルド: `cmake --build <build> -j2 -- -k 0` → 852/852、エラー 0 (`-Werror` のまま)。
  ビルドログの warning 1 件は別リポ (Curare) のファイル名を指す行で、共有 ccache から出たもの。Discutere のソースの警告ではない。
- ctest: `ctest -j2 --output-on-failure` → 209 本中 208 通過、`infra.ws_limits` が失敗 (偽サーバが close に応答せず
  Beast が close の応答を待つため 3 秒で判定できなかった。テスト側の偽サーバを直した)。直した後に
  `ctest -R infra.ws_limits` → 1/1 通過 (6 ケース)。**直した後に全 209 本を通しでは再実行していない** (変えたのはそのテストの .cpp だけ)。
- 実際のコンパイルコマンド (`compile_commands.json`) で確認:
  - `src/main.cpp` / `src/flow/design_gap.cpp` (di_app): `-ftrivial-auto-var-init=zero -D_FORTIFY_SOURCE=3 -fstack-protector-strong
    -fstack-clash-protection -fcf-protection=full -D_GLIBCXX_ASSERTIONS`、unsafe_api.hpp の強制 include と `-Werror=deprecated…`。
  - `sqlite3.c` (di_sqlite): 上の守りのうち C 向けのもの (EXTENDED の zero init / FORTIFY 3 を含む)。unsafe API 拒否は無し (C)。
  - `tests/base/json_tests.cpp`: baseline + unsafe API 拒否。
  - 守りを打ち消すフラグ (`-fno-stack-protector` `-no-pie` 等) は見当たらない。
- `python3 /home/user/Lorica/tools/verify_binary.py`:
  `bin/discutere-server`、`tests/di_test_base_json`、`tests/fuzz/di_fuzz_regress_json` → いずれも
  「ELF PIE/NX-stack/full-RELRO metadata present (instruction coverage not verified)」。他のテスト実行ファイルは未点検。

## (b) 済んだが確かめていないこと

- ASan+UBSan / TSan のビルドとテスト (`-DDI_SANITIZE=...`、既存の option) は今回 **実行していない**。
- `-DDI_FUZZ=ON` (Clang + libFuzzer) の configure / ビルド / 短時間の fuzz は **実行していない**。GCC での回帰 (`fuzz.<name>` 5 本) は ctest で通過。
- Clang での通常ビルド、MSVC / Windows のビルド (環境が無い)。
- submodule (`lib/lorica`) を実際に取得した状態での configure (この環境では submodule 未取得のため `LORICA_SOURCE_DIR` を使った)。
- 本社での結合テスト・実機確認。

## (c) 途中のこと

無し (WIP コミット無し)。

## (d) 未着手のこと

- sanitizer 付きのビルドとテストの実行、`DI_FUZZ=ON` の動作確認 (数十秒)。
- 最終報告 `/home/user/.cpp-deps/reports/Discutere-report.md` (指示書 11 章の形) は未作成。
- port-notes への反映 (この文書で代用)。
- Slack の payload 文字列の入れ子 JSON を同じ budget で数える改善 (今は WS 層・外側 JSON・内側 JSON で別の budget。どれも 4MiB の上限内)。
- Gateway の受信も WS 層と JSON 層で budget が別 (同じ上限値 8MiB)。1 依頼 1 budget に寄せるかは要検討。

## (e) 分かっている失敗

- 無し (上の `infra.ws_limits` は直して単独で通過)。全体の再実行は未。

## (f) 人間の判断が要ること

1. Lorica の取り込み方: 指示書 3.1 の 2 つの形 (FetchContent の固定 / find_package) ではなく、main の 4833ab3 で意図して選んだ
   submodule (`lib/lorica`) を保った。gitlink と `DI_LORICA_COMMIT` を同じ 5e8281b にし、HEAD を照合する。FetchContent に戻すか要判断。
2. SQLite amalgamation の取得元が npm レジストリ (`better-sqlite3-13.0.3.tgz`、SHA256 固定)。sqlite.org はこの環境のプロキシで 403 だった
   (前回の記録)。変えていない。
3. `DI_LORICA_PROFILE=auto` は EXTENDED が使えない環境で baseline に下がる (STATUS で理由を出す)。本社の環境で必ず EXTENDED に
   したいなら `-DDI_LORICA_PROFILE=extended` を既定にするか判断。
4. コミットの末尾の Co-Authored-By はこのセッションの指定の行を使った (COMMON.md の行とモデル名が違う)。
5. 新しいパス (`native/cmake/DiHardening.cmake`, `native/docs/cpp-lorica-20261111/`) は `spec/domains` に登録していない
   (既存の native/cmake・native/docs も未登録。`native/tests/**` は test-support に入っている)。

## (g) Lorica の適用表と上限チェック (いま時点)

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
| Kuzu | C++ ビルドに入っていない (常駐サーバは Kuzu グラフを開かない) | | |

上限チェック (この job で入れた / 確かめた所):

| 経路 | 場所 | 部品 | 上限と根拠 |
|---|---|---|---|
| Discord Gateway / Slack の WS フレーム | `native/src/infra/ws_client_impl.hpp` websocket_handshake (pmd 無効 + `read_message_max`)、on_read (`input_budget`) | input_budget / Beast | Gateway 8MiB・Slack 4MiB (既存の値)。圧縮は交渉しない → RSV1 は断る |
| Gateway の JSON | `native/src/discord_hook/gateway_client.cpp` (parse_json) | input_budget (parse_json 内) | 8MiB / 500,000 要素 / 深さ 64 (既存) |
| SQLite から読む値 | `native/src/base/sqlite.cpp` get / all / each | input_budget | 512MiB・200 万行 (既存の QueryLimits)。get/each も呼び出し側で絞れる |
| S3 バックアップ | `native/src/backup/s3.cpp` s3_part_count / s3_part_range / s3_xml_tag / read_range / ETag | checked_multiply / checked_add / contains_range / input_budget | part 10,000 (S3 の仕様)、ETag 1024 バイト (S3 の ETag は 34 バイト、余裕を持たせた値)、読み出しはファイルの大きさ・part 数ちょうど |
| `claude -p` の出力 | `native/src/persona_engine/llm/claude_cli.cpp` (max_stdout → `base/process.cpp` の input_budget、parse_json) | input_budget | 16MiB (lib/one_shot の既定のまま)、設定可能に |

## (h) 次にやる順番

1. 全 ctest を通しで再実行 (209 本)。
2. ASan+UBSan のビルドと ctest、TSan のビルドと ctest (結果を記録したらビルドディレクトリを消す)。
3. Clang で `-DDI_FUZZ=ON -DDI_SANITIZE=address,undefined` を configure / ビルドし、各 `di_fuzz_<name>` を 30 秒ずつ回す。
4. 他の実行ファイルの `verify_binary.py`。
5. 最終報告 (指示書 11 章の 1〜11) を書く。

## (i) ローカルのビルドとテストの手順

```
cd <repo>
git submodule update --init lib/lorica          # または -DLORICA_SOURCE_DIR=<5e8281b の Lorica checkout>
export CCACHE_DIR=/home/user/.cpp-deps/ccache   # 任意
cmake -S native -B <build> -G Ninja -DCMAKE_BUILD_TYPE=Release \
  [-DLORICA_SOURCE_DIR=/home/user/Lorica] \
  [-DFETCHCONTENT_SOURCE_DIR_BOOST=/home/user/.cpp-deps/src/boost-1.87.0] \
  [-DFETCHCONTENT_SOURCE_DIR_JSON=/home/user/.cpp-deps/src/nlohmann_json/json] \
  [-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache] -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
/home/user/.cpp-deps/bin/buildslot cmake --build <build> -j2
cd <build> && /home/user/.cpp-deps/bin/buildslot ctest -j2 --output-on-failure
python3 <Lorica>/tools/verify_binary.py <build>/bin/discutere-server
```

option: `DI_LORICA_PROFILE` (auto/extended/baseline)、`DI_DEPRECATE_UNSAFE_APIS` (ON)、`DI_LORICA_ALLOW_OTHER_COMMIT` (OFF)、
`DI_SANITIZE` (`address,undefined` / `thread`、Debug で `-O1 -g1` 推奨、native/README.md)、`DI_FUZZ` (Clang のみ、OFF)。
SQLite は configure 時に npm レジストリから取得 (SHA256 固定)。フルビルドは約 850 ステップで、-j2 で数十分かかる。
