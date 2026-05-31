# Discutere セットアップガイド (用途別)

「○○するための設定」を用途別にまとめたインデックス。各ガイドは独立して読める。
設定の全体像 (config ファイル / env / 優先順 / 全キー表) は
[`config-reference.md`](config-reference.md) を参照。

| やりたいこと | ガイド |
|---|---|
| Discord で議論を動かしたい (最短) | [discord.md](discord.md) |
| AI に自走議論させたい (LLM backend) | [llm.md](llm.md) |
| 複数サーバ (guild) で運用したい | [multi-server.md](multi-server.md) |
| 学習データを S3 にバックアップしたい | [backup.md](backup.md) |
| 全設定キーを確認したい | [config-reference.md](config-reference.md) |

## 最短起動 (Discord で議論まで)

```sh
git clone https://github.com/LUDIARS/Discutere && cd Discutere
npm install && npm run build

cp discutere.config.example.json discutere.config.json
# discord.botToken / applicationId / guildIds / adminIds / discussionChannelIds を埋める
# (詳細は discord.md)

npm start    # Gateway 接続 + slash 自動登録 + persona-engine attach
```

詳細手順は [discord.md](discord.md) → [llm.md](llm.md) の順に読むとよい。

## 設定の優先順位

すべての設定は `default < discutere.config.json < env` の順で解決される
(`DISCUTERE_CONFIG` で config パス変更可)。秘密情報 (botToken / apiKey / AWS 鍵) は
config に直書きせず env 上書きを推奨。詳細は [config-reference.md](config-reference.md)。

## 関連設計ドキュメント

- 認証モデル / Discord-only の設計判断: [`../../CLAUDE.md`](../../CLAUDE.md)
- S3 バックアップの設計: [`../backup/DESIGN.md`](../backup/DESIGN.md)
- persona-engine: [`../persona-engine/DESIGN.md`](../persona-engine/DESIGN.md)
