# 任意の参考情報と再議論

仕様 ID: DI-OPTIONAL-VOICES-REDISCUSSION

根拠: neco 2026-10-08「ユーザーの声の取り込み」「外部の声無しでも議論」「参考値」「再議論」「学習という状態は存在しない」。

## 受入条件

- Di に既存データがあれば使用し、なければ取得先を確認する。Steam が空・失敗の場合も YouTube を確認する。
- 提示された内容・事前知識を利用できる。事前知識、仮説、合成意見を実ユーザーの声として数えない。
- 外部の声の件数・偏りは参考情報。議論開始の必須条件とせず、「学習」という独立した選択肢・状態へ移さない。
- 材料がなければ Voluptas に収集を依頼し、「受付を待つ」「外部の声なしで開始」を選ぶ。受付は感想の取得完了とは別。
- 依頼と選択を永続化する。送信結果不明は受付済みとせず、再起動・連打でも重複送信しない。待機中は無操作による自動開始を止める。
- 「再議論」で前回の内容から修正用下書きを作る。前回のペーパー・発言・結論は別の議論 ID として保持する。修正後に人間が開始する。

## 実装の分担

- `user-voices/collect.ts` / `runtime.ts`: Di → Steam → YouTube → 既存 Voluptas 感想の照会。取得不能と真の 0 件を混同しない表示。
- `user-voices/reference-policy.ts`: 議論への参考情報の位置付け。
- `user-voices/preparation.ts`: 依頼送信・受付照合の境界、永続化、待機選択。
- `discord-flow-runs.ts`: スレッドと現在の議論 ID の対応。再議論の草案作成はトランザクションで行う。
- `discord-live.ts`: 声なし開始・受付待ち・受付確認・再議論の回答を処理する。
- 既存の収集・埋め込み関数や過去データ内の `learning` という内部識別子は、利用者向けの状態として使用しない。

## 接続契約

Voluptas の現行 `GET /api/personas/impressions` は既存感想の取得口であり、収集依頼の受付口ではない。
neco の「受付APIを追加」により、`POST /api/personas/impression-requests` と `GET /api/personas/impression-requests/:requestId` を使用する。
`VoiceRequestPort` は `submit({requestId, theme})` と `status(requestId)` を必要とする。
`request-client.ts` が既定接続を提供する。Cernere の `impression-requests:write` 権限を使用し、読み取り用固定トークンへフォールバックしない。
Vo の停止・接続不能、認証失敗、設定不備を区別して表示する。外部の声なしで議論は可能。

新規 API の場合、Di が生成する安定した依頼 ID による冪等受付、受付状態の照合、適切なサービス認証、
収集対象・依頼元の記録が必要。HTTP 応答だけから人間による受付や収集完了を推定しない。

## 検証記録

- TypeScript 型チェック、差分チェック: 実施。
- Di 優先、Steam 0 件から YouTube、送達不明の重複抑止、待機選択の永続化、再議論で旧記録を保持する回帰ケースを追加。
- session によるテスト実行・サービス再起動: 未実施。
- Voluptas 受付 API と Cernere 権限宣言: 別 PR で追加。稼働環境での受付・実 Discord 体験確認は未実施。
