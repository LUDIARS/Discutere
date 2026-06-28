# 学習データ S3 バックアップ — DESIGN

Discutere の学習データ (Discatier KG + persona-engine.db + discutere.db) を tar.gz に
まとめて S3 にアーカイブする。月次自動 + 手動 (slash / npm script) の 2 系統。
実装: `src/backup/`、設定: `config.backup.*`、PR #42 (2026-05-31)。

## 目的 / 要件

- 学習データ (議論 KG・ペルソナ発火状態・タスク等) を喪失から守る。
- 「使うのは S3 のアーカイバ」: `storageClass=GLACIER` 系で安価に長期保管。
- **月次**自動 + **手動** (`npm run backup` / `/discutere-backup`) の両方。
- MinIO 等の S3 互換ストレージにも `endpoint` で流せる。

## 対象データ

`resolveTargets(config)` が config から導出する (存在するものだけアーカイブ、欠損は skip):

| ターゲット | 既定パス | 内容 |
|---|---|---|
| Discatier KG | `config.discatier.kuzuPath` (`./data/discatier.kuzu`) | 議論グラフ本体 |
| persona-engine db | `config.personaEngine.dbPath` (`./data/persona-engine.db`) | ペルソナ/ルール/発火カウンタ |
| discutere db | `DATABASE_PATH` (`./data/discutere.db`) | flow / facilitator-directives / conclusion-cache 等のサイドカー表 |

cwd 相対パスで格納するので、展開すれば元のレイアウトに復元できる。

## モジュール構成 (SRP)

```
src/backup/
├── archive.ts   # createArchive() — 対象を tar.gz 化 (tar パッケージ、gzip+portable)
├── s3.ts        # uploadFileToS3() — PutObject (StorageClass / endpoint / 認証)
└── runner.ts    # runBackup() + startBackupScheduler() + state I/O
```

- `runBackup(config)`: tmp に tar.gz 生成 → S3 push → `stateFile` 更新 → tmp 削除。
  bucket 未設定なら `ok:false` を返す (throw しない)。key は
  `${prefix}${workspace}/discutere-backup-YYYYMMDD-HHmmss.tar.gz`。
- `startBackupScheduler(config)`: `enabled` かつ `bucket` 設定時のみ稼働。起動 30 秒後に
  overdue チェック + 以後 24h tick で `intervalDays` (既定 30) 経過判定。`trigger()` を
  手動経路 (slash / API) と共有。

## トリガ経路

| 経路 | 実装 | 認可 |
|---|---|---|
| 自動 (月次) | `startBackupScheduler` (index.ts で起動) | — |
| 手動 script | `npm run backup` → `scripts/backup.ts` | ローカル実行者 |
| 手動 slash | `/discutere-backup` → `command-router` `handleBackupSlash` | admin allowlist |

slash は tar+upload が数秒かかり得るため **fire-and-forget で即時 ack** し、結果は
サーバログ / dashboard で確認する (Discord interaction の 3 秒 ack 制限回避)。

## 再起動耐性

最終実行時刻は `config.backup.stateFile` (既定 `./data/backup-state.json`) に
`{ lastRunAt, lastKey }` で永続化。スケジューラは `now - lastRunAt >= intervalDays`
で判定するので、プロセス再起動を跨いでも月次間隔が保たれる (二重実行しない)。

## AWS 認証情報の解決 (重要)

`s3.ts` は **`accessKeyId` と `secretAccessKey` を両方明示した時だけ** それを使い、
未指定なら `new S3Client({ region })` が **AWS SDK v3 のデフォルト認証チェーン** に
フォールバックする。これは AWS CLI と同じ探索順:

1. 環境変数 (`AWS_ACCESS_KEY_ID` / `AWS_SECRET_ACCESS_KEY` / `AWS_SESSION_TOKEN`)
2. **共有ファイル `~/.aws/credentials` / `~/.aws/config`** (= `aws configure` が書く場所)
3. SSO / EC2・ECS の IAM ロール 等

→ **`aws configure` 済みなら、config に鍵を書かずバックアップが動く** (`backup.enabled`
と `backup.bucket` の設定のみで可)。鍵を config / env に直書きしない運用を推奨。

### 注意点

- **region は config 側が優先**: コードは常に `region` を明示的に S3Client へ渡すので
  `~/.aws/config` の region は使われない。**バケットのリージョンに `backup.region` を
  合わせる** (既定 `ap-northeast-1`)。ズレると `PermanentRedirect` で失敗する。
- **名前付きプロファイル**: `default` 以外を使う場合は `AWS_PROFILE=<name>` を環境に渡す。

## 設定リファレンス

`default < discutere.config.json < env` の順で解決。

| config キー | env | 既定 | 説明 |
|---|---|---|---|
| `backup.enabled` | `DISCUTERE_BACKUP_ENABLED` | `false` | 自動スケジューラの有効化 (手動は常に可) |
| `backup.bucket` | `DISCUTERE_BACKUP_BUCKET` | — | S3 bucket (未設定なら不可) |
| `backup.region` | `DISCUTERE_BACKUP_REGION` / `AWS_REGION` | `ap-northeast-1` | バケットのリージョン |
| `backup.prefix` | `DISCUTERE_BACKUP_PREFIX` | `discutere/` | key prefix |
| `backup.storageClass` | `DISCUTERE_BACKUP_STORAGE_CLASS` | `GLACIER` | `DEEP_ARCHIVE` / `STANDARD_IA` / `STANDARD` 等も可 |
| `backup.endpoint` | `DISCUTERE_BACKUP_ENDPOINT` | — | S3 互換 (MinIO 等) |
| `backup.intervalDays` | `DISCUTERE_BACKUP_INTERVAL_DAYS` | `30` | 自動周期 (= 月次) |
| `backup.stateFile` | `DISCUTERE_BACKUP_STATE_FILE` | `./data/backup-state.json` | 最終実行記録 |
| (認証) | `AWS_ACCESS_KEY_ID` / `AWS_SECRET_ACCESS_KEY` | — | 未指定なら SDK 既定チェーン |

## 運用手順

```sh
# 1. AWS CLI 設定済みなら認証は自動 (未設定なら env か config に鍵)
aws configure   # default プロファイル

# 2. config を設定
#   backup.enabled=true / backup.bucket=<bucket> / backup.region=<bucketのregion>

# 3. 疎通確認 (手動 1 回)
npm run backup
#   → ✅ uploaded s3://<bucket>/discutere/<ws>/discutere-backup-...tar.gz

# 4. あとは起動中プロセスが月次自動実行。手動は /discutere-backup (admin) でも可
```

## 復元 (リストア)

S3 から tar.gz を取得し、リポジトリルートで展開すれば `data/` 配下に元のファイルが戻る。
Glacier 系は取り出しに時間がかかる (DEEP_ARCHIVE は数時間) ので、即時復旧が要る運用では
`STANDARD_IA` 等を検討する。

```sh
aws s3 cp s3://<bucket>/discutere/<ws>/<file>.tar.gz .
tar xzf <file>.tar.gz    # data/discatier.kuzu, data/persona-engine.db, data/discutere.db が復元
```

## テスト

- `npm run test:backup` (`tests/backup/run.ts`): `createArchive` が存在ファイルのみ含め、
  欠損を skip し、対象 0 件で throw することを確認 (S3 には触れない)。
- S3 アップロード経路は認証情報が要るため自動テスト対象外。`npm run backup` で疎通確認する。
