# Vault 注入環境変数による起動

関連: `actio:1e931308-775c-4f7e-bdb2-4dbfd7f81d08`

## 契約

C-1 getYoutubeApiKey(env): 注入環境変数のみを参照し、未設定なら null を返す
C-2 getYoutubeApiKeyStatus(env): キー本文を含まず設定有無だけを返す

## 変更単位

- 起動スクリプトの dotenv/env-cli を撤去し、catalog の既存ポートを維持する。
- YouTube キー取得を Ex 注入 env に統一し、Infisical と gcloud からの保存経路を削除する。
- 設定 UI は設定有無のみを表示する。キーの変更は Ex Vault で行い、次回起動で反映する。
- 既存の production startup guard と YouTube 利用時の未設定拒否を再利用する。YouTube は任意機能のため、キー未設定をサービス全体の必須条件にはしない。
- README・設定仕様・運用ガイドを更新し、実際の `.env` / `.env.secrets` は操作しない。

## 検証計画

`vault-only-startup-test-plan.json` は Augur の計画。対の回帰テストは
`tests/secrets/youtube-env.test.ts` と `tests/secrets/tuning-secrets.test.ts`。
空/空白/設定済み env、秘匿された状態表示、起動コマンド、旧更新 API の撤去を確認する。
テスト・サービス起動は委託本文の指示で実行せず、審査に委ねる。
契約注入は Anatomia の workspace 外ログ書込みが EPERM となり未実施。
契約の実行証跡は未取得として集計結果をそのまま報告する。
