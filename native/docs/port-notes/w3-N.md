# port-notes w3-N — 外部の声の検索 (RAG)・Discatier アダプタ・イベントブリッジ・core のコマンド処理

job `cpp-node-20261030`。担当 TS 21 ファイル (cross の 7 ファイルを含む) をすべて C++ に移した (Node に残したものは無い)。
golden 生成: `npx tsx native/tests/golden/w3-N-golden.gen.ts` (出力 `native/tests/fixtures/w3n/*.json`、値はすべて架空。
DB は一時ディレクトリに組んだ合成データで、表を rowid 順に記録し C++ のテストが同じ行を入れて同じ操作をする)。

## 1. 機能の対応表 (TS → C++)

| TS | C++ | 備考 |
|---|---|---|
| src/discatier-engine-adapter/voice-search.ts | native/src/discatier_engine_adapter/voice_search.{hpp,cpp} | `search_external_voice_rows` / `build_search_terms`。温めの埋め込みクライアントは設定から 1 つ作る (`set_voice_embedder_for_test` で差し替え可) |
| src/discatier-engine-adapter/index.ts | discatier_engine_adapter/index.{hpp,cpp} | `create_discatier_context_provider(core, options)` → `shared_ptr<DiscussionContextProvider>` |
| src/discatier-engine-adapter/event-bridge.ts | discatier_engine_adapter/event_bridge.{hpp,cpp} | `create_event_bridge(core, BridgeEngine, options)`。`bridge_engine_of(PersonaEngineHandle)` で実 engine を渡す |
| src/flow/external-voices.ts | flow/external_voices.{hpp,cpp} | `make_list_external_voices(open_core, ws)` → `ListExternalVoicesFn` |
| src/visualize/md-exporter.ts | visualize/md_exporter.{hpp,cpp} | `export_*` / `export_node` / `exporter_for` / `ExporterError`。front matter は `lib/front_matter::stringify` |
| src/visualize/dump.ts | visualize/dump.{hpp,cpp} | `dump_node` / `resolve_file_path` |
| src/core/projection/types.ts | core/projection/types.hpp | `ReturnTypeCreateCore = Core` |
| src/core/projection/command-handlers/types.ts | core/projection/command_handlers/types.{hpp,cpp} | `HandlerContext` (core はポインタ) / `HandlerResult` / `latest_hypothesis_id` / `set_session_mode` / `append_event` |
| …/cross/{cluster,find,hotspot,index,lineage,me-role,stale}.ts | …/cross/{cluster,find,hotspot,index,lineage,me_role,stale}.{hpp,cpp} | `run_uncategorized_clustering_job` / `run_hotspot_aggregation_job` も同じ場所 |
| …/emotion/index.ts, learning/index.ts, synthesis/index.ts | …/emotion/index.*, learning/index.*, synthesis/index.* | |
| src/core/hypothesis/lifecycle-handler.ts | core/hypothesis/lifecycle_handler.{hpp,cpp} | `apply_lifecycle` は `TransitionResult` を返す |
| src/core/hypothesis/validation-routing.ts | core/hypothesis/validation_routing.{hpp,cpp} | |
| src/discord-hook/auto-discussion.ts | discord_hook/auto_discussion.{hpp,cpp} | `create_discord_auto_discussion_starter` は同期関数を返す |

## 2. テストの対応表 (TS テスト → ctest 名)

| TS テスト | ctest |
|---|---|
| tests/persona-engine/external-voices.test.ts, external-voices-lookup.test.ts (adapter・部分索引) | discatier_engine_adapter.voice_search (+ golden: 15 クエリの順位とスコア・adapter 4 件、温め、境界、external-voices の配線、機密/内部の遮断) |
| tests/persona-engine/discatier-adapter.test.ts, find-discussion-session.test.ts, auto-export.test.ts, event-bridge.test.ts, event-bridge-discord.test.ts, human-utterance.test.ts | discatier_engine_adapter.adapter (+ web scene・payload の sessionId 優先・poll の上限・巨大 payload・fire の例外・scheduler の start/stop) |
| tests/visualize/md-exporter.test.ts | visualize.md_exporter (+ golden 20 件を 1 バイト一致、dump、パスの検査、refs_json の上限) |
| tests/discord-hook/auto-discussion.test.ts | discord_hook.auto_discussion (+ golden: fallback 12・LLM 12 (プロンプトも一致)・開始 5、応答の上限ちょうど/+1・深さ・壊れた JSON・巨大 evidence) |
| tests/core/hypothesis/lifecycle.test.ts | core.command_handlers (+ golden: 35 コマンドの応答と積んだイベントの payload) |

## 3. 安全の対応表

| 入力経路・防御 | C++ |
|---|---|
| 外部の声の個人アンカー | `speaker_id` は `masked_persona_label` (`論者#xxxxxx`) にして露出、公開 ID は出さない。出所 (source / sourceUrl) は attribution から開示。golden とテストで speakerLabel に `ext:` / 公開 ID が含まれないことを確認 |
| `機密` / `内部` タグ | 呼び出し側の `flow::gate_external_voices` (w1-E) が lookup ごと外す。`make_list_external_voices` と組み合わせた境界テスト (機密・内部は nullopt、運用/開発/未来予測/タグ無しは通す) |
| LIKE の照合 | `% _ \` を `\` でエスケープして `ESCAPE '\'` (TS と同じ)。全件照合 (直近 N 件制限なし) で候補は `LIMIT 2000` |
| DB の値 (候補・games・一覧・events・evidence_json・refs_json・payload_json) | `sql::QueryLimits` で行数・バイト数を課金、JSON は `parse_json` の limits (payload/evidence 8MiB・1MiB、refs 4MiB) |
| クエリ埋め込みの温め | `background_executor` (満杯なら warn して温めない、キーワード検索で継続)。埋め込みクライアントを作れない設定は warn して継続 |
| event-bridge の poll | `global_scheduler().every` で起き、poll 本体は `background_executor` (走行中なら飛ばす)。1 回の poll は 5,000 件まで (末尾と同じ created_at の行は次回に回して取りこぼさない) |
| auto-discussion の LLM 応答 | 256KiB を超える応答は読まない、JSON は 256KiB・要素 1 万・深さ 32。`/\{[\s\S]*\}/` は最初の `{` から最後の `}` までの線形の切り出し。fallback の語判定は部分一致 (正規表現なし) |
| dump の出力パス | `fs::safe_join` で root の外 (`..`・絶対パス) を拒否 (移植で追加) |
| md の時刻 | 1970〜9999 年の外は `std::range_error` (TS は RangeError か別書式) |

## 4. 移していないもの・挙動の差

- **Node に残したものは無い**。
- ctest の persona_engine.worker_pool (担当外) が全体実行で 1 回落ち、単独では通った (時間に依存する不安定なテストらしい)。
- `onPostedUtterance` / `onPostedHypothesis` は呼んだスレッドで同期に呼ぶ (TS も同期部分は即時に走り、async 部分は投げっぱなし)。
  Discord 投稿など重い I/O は callback 側で `background_executor` に渡すこと。callback の例外は TS の `.catch` と同じく
  `onAutoExportError` に流す (TS で同期に投げた場合は呼び出し元へ伝播していた)。
- auto-export (md 書き出し) は TS の dynamic import 後 (次の tick) ではなく、投稿の直後に同期で行う (結果は同じ、失敗は onAutoExportError)。
- event-bridge の engine は `BridgeEngine` (fire / reset / personas / rules) で受ける。personas/rules が無い engine で終結イベントを
  受けると「記録できない」を on_error に流す (TS は TypeError を on_error に流していた。結果は同じ)。
  `stop()` は走行中の poll の終わりを待つ (stop の後に core / engine を閉じてよい)。
- event-bridge の 1 回の poll の上限 (5,000 件) は移植で追加。全行が同じ created_at の場合だけは切らずに全部処理する。
- auto-discussion の LLM 応答で title 等が文字列でない (数値など) と TS は TypeError で失敗する。C++ も `std::runtime_error` で同じく失敗にした。
- auto-discussion の LLM 応答が 256KiB を超えると読まずに fallback (TS は全体を正規表現にかけていた)。
- md-exporter: refs_json が上限 (4MiB) を超えると壊れた値と同じく []。created_at 等が 1970〜9999 年の外だと例外。
- dump: root の外を指す workspace / id は `std::invalid_argument` (TS は外に書いていた)。
- external-voices: `open_core` の例外は TS と同じく呼び出し側へ投げる。検索中の例外は空配列。
- voice-search: 埋め込みクライアントを作れない設定 (URL 不正) は TS では例外だったが、warn してキーワード順で返す。

## 5. 土台に欲しいもの / 人間の判断が要ること

- 作業環境のディスクが逼迫している (`/` の空きが 1GB を切る時間帯があり、Debug (-g) のフルビルドが `No space left on device` で落ちた)。
  さらに途中で空きが数十 MB まで減ったため、このビルドディレクトリは `/dev/shm/build-w3-N` (tmpfs) に置き、
  `-DCMAKE_CXX_FLAGS_DEBUG=-O0` (デバッグ情報なし) で configure、`TMPDIR=/dev/shm/...` でビルドとテストを回した。
  `/tmp` には blackbox の jsonfile テストが残したらしい `bb-file-*` (1 個 65MB、90 個) も溜まっている。古い担当のビルドディレクトリ
  (`/home/user/build-w1-*`, `build-w2-*`) の整理か、`tools/build.sh` に既定でデバッグ情報を落とすオプションがあると助かる。
- `Scheduler::every` の仕事を executor に渡す「走行中なら飛ばす」定型 (event-bridge で書いた) は他のポーリングでも使うので、土台に
  `every_on_executor(interval, fn)` のような部品があると重複が減る。
- 人間の判断: auto-discussion の LLM 応答で title 等が文字列でないとき、TS どおり失敗にするか (現状)、項目なしとして扱うか。
