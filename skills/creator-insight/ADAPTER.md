# Discutere での使い方

このファイルだけがリポジトリ固有。ほかのファイル（コア）は導入先で共通にする。
コアの正本は当面このリポジトリに置く。コアを変えるときはここで変え、ほかの導入先へ同じ内容を反映する。

## Discutere での位置づけ

- エージェントが読んで実行するスキルであり、サーバ（`native/`）の機能ではない。slash command、HTTP の経路、DB のテーブルを足さない。
- 解釈は、スキルを実行しているエージェント自身が行う。サーバの LLM backend、worker-pool、ペルソナエンジンを呼ばない。
- 議論のペルソナ（論者、代理議論の人物パック）とは別物。このスキルの結果をペルソナの材料にしない。

## 使える入力

| 入力 | 取り方 |
|---|---|
| チャットログ | 本人が持ち込んだ Discord / Slack のエクスポート。管理者がエクスポートする場合は、対象者ごとに同意を取る |
| コード | 本人が指定したリポジトリ |
| ポートフォリオ | 本人が渡した URL またはファイル |
| アンケート | `data/life-script-questionnaire.ja.json` への本人の回答 |

Discutere の KG と SQLite は、発言者の名前やアカウントを保存しない設計になっている（匿名 workspace）。
**Discutere の DB から個人別のログを作らない。** 監査用に保存している `author_id` も使わない。

## 書き戻さない

- KG、`discussion_paper`、`flow_utterance`、ペルソナプールへ、個人の分析結果を書かない。
- 特徴カードと対話用メモを、バックアップ（S3）や Fundamentum export の対象ディレクトリに置かない。
- 作業ファイルはリポジトリの外に置く。`data/` 配下にも置かない。

## 語彙

- プレイタイプの15軸は、Voluptas の `PREFERENCE_AXES` が正本。表示名は `src/flow/persona-descriptor.ts` の `AXIS_LABELS` に合わせてある。軸を足さない。
- メカニクス辞書は `data/games/` のメカニクス定義と、Ludus game-lexicon 由来の用語辞書（`ludus-term-aliases`）を使う。
- ゲーム名の略称は `game-aliases` の別名展開に合わせる（「モンスト」と正式名を同じタイトルとして数える）。

## ツールの置き場所

| 種類 | 場所 |
|---|---|
| 集計ツール | `scripts/creator-insight/<名前>.ts`（`tsx` で実行） |
| 索引 | `scripts/creator-insight/README.md` |
| テスト | `tests/creator-insight/<名前>.test.ts`、まとめて `tests/creator-insight/run.ts` |
| npm scripts | 実行は `insight:<名前>`、テストは `test:creator-insight` |

- テストを足したら `.github/workflows/ci.yml` のテスト一覧にも足す。
- C++ 版サーバ（`native/`）には入れない。
- 仮名化の鍵は環境変数 `DISCUTERE_CREATOR_INSIGHT_HMAC_KEY`。値は Ex Vault で管理し、コード・ログ・設定ファイルに書かない。

## 外部への送信

- エクスポートしたログの本文を、外部の API に送らない。
- LLM に渡すのは、特徴カードと、同意のある場合の引用（20件、各120字、仮名化済み）だけ。
- 引用を渡してよいかは、実行前の確認で本人に聞く（`consent.llmQuotes`）。

## 仕様

受入条件とデータの保持は `spec/feature/creator-insight.md`。
