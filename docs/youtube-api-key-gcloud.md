# YouTube API キーの Vault 管理

`DISCUTERE_YOUTUBE_API_KEY` は Excubitor Vault に登録し、`discutere` に紐付ける。
Discutere は Ex が起動時に注入する `process.env` のみを参照する。
非秘密の設定は `excubitor.catalog.yaml` の `env` で管理する。

設定画面の YouTube 欄は現在のプロセスでキーが設定済みかだけを表示する。
キー本文の表示・入力・保存、gcloud 取込、アプリからの秘密情報再取得は行わない。
Vault の変更は Ex からの次回起動で反映される。

キー未設定でも YouTube 以外の機能は使える。YouTube クロールを要求した場合は
`DISCUTERE_YOUTUBE_API_KEY` の未設定エラーとなる。
旧 `.env` / `.env.secrets` の移行確認・削除は管理者の別作業とする。
