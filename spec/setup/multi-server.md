# 複数サーバ (guild) で運用するための設定

複数の Discord サーバの議論を **1 つの workspace (= 単一 Discatier KG) に集約**する。

## 仕組み

- 学習データ (Kuzu KG) は `discatier.kuzuPath` **単一**に集約される。
- 議論 session は `discord:<guildId>/<channelId>` で **guild 別に分離**される
  (post 先が混ざらない) が、KG は 1 つ。
- AI 発話の post 時は **guild ID 一致**で bot を選ぶので、同じ bot を複数 guild に
  置いてもクロス post しない。

## 設定

```jsonc
"workspace": "knowledge",           // 集約先 (全 guild で共通)
"discord": {
  "guildIds": ["G1", "G2", "G3"],   // ← 運用する全 guild を列挙
  "discussionChannelIds": ["C1", "C2"]
}
```

`guildIds` に列挙した各 guild に:

- slash command を **guild commands として即時登録**する
- discutere-monitor の状態カードを設置する

> env では `DISCUTERE_DISCORD_GUILD_IDS=G1,G2,G3` (カンマ区切り)。
> 旧 単数 `guildId` (`DISCUTERE_DISCORD_GUILD_ID`) は後方互換で `guildIds` に統合される。

## bot 構成の選択肢

| 構成 | 設定 | 備考 |
|---|---|---|
| 1 トークンを複数 guild に invite | `guildIds` に全 guild | 最もシンプル。monitor 行は guild ごとに作る |
| guild ごとに別 bot | monitor 行ごとに `botToken` | 各 monitor 行が自身の token を持ち、guild ID で選別 |

どちらでも動く。monitor 行 (`/api/groups/:ws/monitors`) は guild ごとに登録し、
`botToken` / `botWorkspaceId` (= Guild ID) を持たせる。

## workspace を分けたい場合

guild ごとに学習データを**分離**したい (集約しない) 場合は、`DISCATIER_WORKSPACE` を
変えて別プロセスで起動する。1 プロセス = 1 workspace = 1 KG。

Discord 側の基本設定は [discord.md](discord.md) を参照。
