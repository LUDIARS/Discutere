# worker-home — 常駐ワーカーの起動ディレクトリ

Discutere の `backend=worker-pool` で spawn される各ペルソナワーカー
(`lictor claude --model <m> --permission-mode acceptEdits`) の **cwd** がここ。

ワーカーを Discutere 本体リポの heavy な開発ルールから隔離し、
専用の `.claude/settings.json` (edit-mode + node スクリプトの allow-list) だけを効かせる。

## 構成

```
worker-home/
├── .claude/
│   └── settings.json     # defaultMode=acceptEdits + register/send スクリプトを allow-list
├── scripts/
│   ├── register.mjs      # 起動直後の自己 register (旧: 生 curl)
│   └── send.mjs          # 1 ターンの発話を POST   (旧: 生 curl)
├── replies/              # ワーカーが書く返信 JSON (gitignore)
└── README.md
```

## なぜ curl をやめたか

旧実装は standing prompt 内に生 curl を埋めていたが、auto-mode の権限分類器が
「内部オーケストレーション (127.0.0.1:3100) への POST + ファイル指示の自走」を
プロンプトインジェクションとみなして遮断した。議論ワーカーに特殊な権限は不要なので:

1. curl → **allow-list 済みの node スクリプト** (`register.mjs` / `send.mjs`) に置換。
2. 起動を auto-mode ではなく **edit-mode (`acceptEdits`)** に変更。
3. mode と許可スクリプトを **`.claude/settings.json`** で宣言。
4. claude の cwd をこの専用ディレクトリにして、専用 `.claude` を効かせる。

## scripts が使う環境変数 (spawner / Lictor sidecar が注入)

| 変数 | 用途 |
|---|---|
| `DI_CALLBACK_URL` | Discutere base URL (例 `http://127.0.0.1:3100`) |
| `DI_WORKER_ID`    | ワーカー / ペルソナ id |
| `LICTOR_PORT`     | ワーカー自身の Lictor sidecar port (register 用) |

## .claude/settings.json (要配置 — テンプレートから手動コピー)

このファイルは権限 allow-list を含むため、auto-mode の権限分類器が
「権限拡大 / 分類器バイパス」とみなして自動生成ツール (AI の Write/Edit) から
弾くことがある。そのため**有効な設定そのものはコミットせず、テンプレート
`settings.template.json` を同梱**しておき、運用者が手動で `.claude/settings.json`
にコピーして有効化する:

```powershell
New-Item -ItemType Directory -Force "worker-home\.claude" | Out-Null
Copy-Item "worker-home\settings.template.json" "worker-home\.claude\settings.json"
```

（`settings.template.json` の `_README` キーは Claude Code が無視するので、コピー後そのままでも動く。
気になるなら `_README` 行だけ削ってもよい。）

中身（最低限）:

```json
{
  "permissions": {
    "defaultMode": "acceptEdits",
    "allow": [
      "Bash(node scripts/register.mjs)",
      "Bash(node ./scripts/register.mjs)",
      "Bash(node scripts/send.mjs:*)",
      "Bash(node ./scripts/send.mjs:*)"
    ]
  }
}
```
