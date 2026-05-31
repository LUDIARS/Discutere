# 学習データを S3 にバックアップするための設定

Discatier KG + persona-engine.db + discutere.db を tar.gz にまとめて S3 (Glacier 系
ストレージクラス) にアーカイブする。**月次自動 + 手動**の両方。
設計の詳細・復元手順は [`../backup/DESIGN.md`](../backup/DESIGN.md) を参照。

## 最短設定

```jsonc
"backup": {
  "enabled": true,                 // 自動スケジューラを有効化
  "bucket": "my-discutere-archive",
  "region": "ap-northeast-1",      // ★ バケットのリージョンに合わせる
  "storageClass": "GLACIER",       // 安価アーカイブ (STANDARD 等も可)
  "intervalDays": 30               // 月次
}
```

## AWS 認証情報

`accessKeyId` / `secretAccessKey` を **両方明示した時だけ**それを使い、未指定なら
**AWS SDK 既定チェーン** (AWS CLI と同じ) にフォールバックする:

1. 環境変数 `AWS_ACCESS_KEY_ID` / `AWS_SECRET_ACCESS_KEY`
2. **`~/.aws/credentials` / `~/.aws/config`** (= `aws configure` が書く場所)
3. SSO / IAM ロール 等

→ **`aws configure` 済みなら鍵を書かずに動く** (`enabled` + `bucket` だけでよい)。

### 2 つの注意点

- **region は config 側が優先**: コードは常に `region` を明示するので、`~/.aws/config`
  の region は使われない。**`backup.region` をバケットのリージョンに合わせる**
  (ズレると `PermanentRedirect`)。
- **名前付きプロファイル**: `default` 以外を使うなら `AWS_PROFILE=<name>` を環境に渡す。

## 実行方法

| 方法 | コマンド | 認可 |
|---|---|---|
| 自動 (月次) | `enabled=true` で起動中プロセスが `intervalDays` ごとに実行 | — |
| 手動 (script) | `npm run backup` | ローカル実行者 |
| 手動 (slash) | `/discutere-backup` | admin (`discord.adminIds`) |

## 疎通確認

```sh
npm run backup
# → ✅ uploaded s3://<bucket>/discutere/<ws>/discutere-backup-YYYYMMDD-HHmmss.tar.gz
```

## 設定キー

| config キー | env | 既定 |
|---|---|---|
| `backup.enabled` | `DISCUTERE_BACKUP_ENABLED` | `false` |
| `backup.bucket` | `DISCUTERE_BACKUP_BUCKET` | — |
| `backup.region` | `DISCUTERE_BACKUP_REGION` / `AWS_REGION` | `ap-northeast-1` |
| `backup.prefix` | `DISCUTERE_BACKUP_PREFIX` | `discutere/` |
| `backup.storageClass` | `DISCUTERE_BACKUP_STORAGE_CLASS` | `GLACIER` |
| `backup.endpoint` | `DISCUTERE_BACKUP_ENDPOINT` | — (MinIO 等の S3 互換用) |
| `backup.intervalDays` | `DISCUTERE_BACKUP_INTERVAL_DAYS` | `30` |
| `backup.stateFile` | `DISCUTERE_BACKUP_STATE_FILE` | `./data/backup-state.json` |
| (認証) | `AWS_ACCESS_KEY_ID` / `AWS_SECRET_ACCESS_KEY` | — |

復元 (リストア) 手順は [`../backup/DESIGN.md`](../backup/DESIGN.md#復元-リストア) を参照。
