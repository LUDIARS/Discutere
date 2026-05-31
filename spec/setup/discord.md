# Discord で議論を動かすための設定

Discutere を Discord ギルド内の議論 ChatBot として動かす最短手順。
transport は **Gateway (WebSocket) 常時接続**で、公開 URL / Interactions Endpoint は不要。

## 1. Discord Application / Bot を作る

1. <https://discord.com/developers/applications> で **New Application**
2. **Bot** タブで Bot を追加し、**Bot Token** を控える (`discord.botToken`)
3. **General Information** の **Application ID** を控える (`discord.applicationId`)
4. **Bot** タブの Privileged Gateway Intents で **Message Content Intent** を ON
   (平文メッセージの取り込みを使う場合に必須)

## 2. Bot を guild に招待する

OAuth2 → URL Generator で以下を選んで生成した URL から招待:

- Scopes: **`bot`** + **`applications.commands`**
- Bot Permissions: メッセージ送信 / メッセージ履歴の閲覧 / リアクション追加
  (議論チャンネルに投稿・👀 リアクションするため)

> Gateway 接続なので **INTERACTIONS ENDPOINT URL の設定は不要**。

## 3. config を書く

`discutere.config.json` (実体は gitignore):

```jsonc
{
  "discord": {
    "botToken": "",            // ← env 上書き推奨 (DISCUTERE_DISCORD_BOT_TOKEN)
    "applicationId": "123...", // slash 自動登録に使う
    "guildIds": ["456..."],    // 運用 guild (複数は multi-server.md)
    "adminIds": ["789..."],    // /discutere-kill /backup を叩ける Discord user id
    "discussionChannelIds": ["100..."]  // 平文を取り込む議論チャンネル
  }
}
```

| キー | 役割 | 空のときの挙動 |
|---|---|---|
| `botToken` | Gateway 接続 | Gateway 起動を skip |
| `applicationId` | slash 自動登録 | `client.application.id` で自動解決 |
| `guildIds` | slash 即時登録 + monitor カード設置 | global 登録 (反映に最大1時間) |
| `adminIds` | admin slash の認可 allowlist | admin コマンドを**全 deny** (安全 default) |
| `discussionChannelIds` | 平文取り込み対象 | **取り込まない** (安全 default) |

## 4. slash command の登録

起動時 (gateway ClientReady) に **自動登録**される。bot を起動せず手動で登録/再登録したい場合:

```sh
npm run discord:register   # discord.botToken + applicationId が必要
```

`guildIds` 指定で **guild commands** (即時反映)、未指定で **global commands** (最大1時間)。

## 5. 起動して使う

```sh
npm run build && npm start
```

| slash | 用途 |
|---|---|
| `/propose statement:<仮説>` | 仮説を提案 → persona が自走議論 |
| `/validate mode:theory\|emotion` | 仮説を検証 |
| `/integrate` / `/reject` | 統合 (採用) / 棄却 |
| `/discutere-queue` | 議論キューを可視化 |
| `/discutere-status` | engine 稼働状態 |
| `/discutere-kill enabled:true\|false` | 自走の停止/再開 (admin) |
| `/discutere-backup` | 学習データを S3 バックアップ (admin) |

## 自然なテキスト取り込み

`discussionChannelIds` に入れたチャンネルでは **slash を打たずとも全平文を議論に取り込む**。

- 取り込んだメッセージには bot が 👀 を付けてフィードバックする。
- **スレッド**の発言は親チャンネルが許可リストにあれば継承して取り込み、AI 返信も同じ場所に返る。
- リスト外のチャンネルは取り込まない (ノイズ防止)。

## トラブルシュート

| 症状 | 原因 / 対処 |
|---|---|
| slash が候補に出ない | 登録されていない → `npm run discord:register`。guild commands は即時、global は最大1時間 |
| 平文が議論に乗らない | 対象 ch を `discussionChannelIds` に入れる + Message Content Intent を ON |
| admin slash が「disabled」 | `adminIds` が空 → 自分の Discord user id を入れる |
| Bot がログインしない | `botToken` 不正 / 未設定。ログの `discord-gateway: skipped` を確認 |

AI に議論させる LLM の設定は [llm.md](llm.md) へ。
