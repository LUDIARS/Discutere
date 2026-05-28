# Discatier Discord Hook Architecture (Concordia-Independent)

## Goal
- `Discatier` を `Concordia` 非依存で動かす。
- Discord と Discatier が直接会話できる構造にする。
- 既存の Discord 受信処理をライブラリ化し、将来の Bot Gateway / Interaction 追加を容易にする。

## Scope (This Implementation)
- `src/discord-hook` を新設し、Discord inbound payload の正規化を共通化。
- `machina` 側の `/webhook/discord` ルートは `discord-hook` ライブラリを経由して処理。
- 既存の `MACHINA` タスク/議論モード処理ロジックは維持。

## Non-Goals (This Phase)
- Discord Gateway 常時接続クライアントの実装
- Slash Commands / Interactions 完全対応
- Concordia 連携の除去（現在そもそも強依存していない）
- Ludus 辞典 API / ソース仕様抽出の本実装

## Design Principles
- Discord transport とドメインロジックを分離する。
- Discord payload 形式差分（Webhook形式、Gateway形式）を入口で吸収する。
- ドメイン層は「正規化済みメッセージ」だけを受ける。

## New Module Boundary

### `src/discord-hook/types.ts`
- `DiscordInboundMessage` など transport 入力型を定義。

### `src/discord-hook/normalize.ts`
- `normalizeDiscordInboundMessage(payload)` を提供。
- 対応入力:
  - Raw Webhook style: `{ id, channel_id, author, content, mentions, timestamp }`
  - Gateway style: `{ t: "MESSAGE_CREATE", d: {...} }`
  - Event wrapper style: `{ event: {...} }`

## Integration Points
- `src/machina/routes.ts`
  - `/webhook/discord` で `normalizeDiscordInboundMessage` を呼ぶ。
  - 正規化成功時のみ `handleDiscordMessage()` に委譲。
- `src/machina/webhook-handler.ts`
  - Discord受信引数型を `DiscordInboundMessage` に統一。

## Why This Fits Discatier
- `Concordia` とは独立に Discord 直結で議論運用できる。
- 今後 `discatier-core` を追加する際、`discord-hook` を再利用して transport 差分を隔離できる。
- 将来 `Lictor` を中継に挟む場合も、入口型を崩さず adapter 追加で対応できる。

## Next Steps
1. `discord-hook` に signature verification / interaction parsing を追加。
2. `discatier-core` を新設し、`MACHINA` 依存ロジックから切り離す。
3. Ludus 辞典クライアントを `discatier-core` に実装し、議論時の参考メカニクス提示を追加。
4. 小規模ゲーム向け `spec-extractor` を実装し、ソース由来仕様の自動提示を追加。

