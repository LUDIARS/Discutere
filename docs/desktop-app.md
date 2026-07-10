# デスクトップアプリ配布 (Electron / Windows exe)

Discutere をローカルアプリ (exe) として配布するためのパッケージング。
**Electron を採用** し、既存の Node サーバ (dist/index.js) を無改変のまま
Electron main プロセス内で起動して、BrowserWindow でトップページ
(`http://127.0.0.1:<port>/`) を開く「ランチャー + 内蔵ブラウザ」構成。

## なぜ Electron か (Tauri でない理由)

- Discutere の本体は **Node サーバ** (Hono HTTP + Discord Gateway) で、
  `better-sqlite3` / `kuzu` という **ネイティブ Node アドオン** に依存する。
- Electron の main プロセスは Node そのものなので、サーバを `import` するだけで動く。
  サーバコードの改変ゼロ。
- Tauri は Rust バックエンドのため、Node ランタイムを sidecar として別途同梱する
  必要があり、ネイティブアドオン込みの Node アプリでは二重管理になる。利点 (バイナリ
  サイズ) より複雑さが勝つため不採用。

## 構成ファイル

| ファイル | 役割 |
| --- | --- |
| `electron/main.mjs` | メインプロセス。データ移設 (シード) → サーバ import → `/health` 待ち → ウィンドウ表示 |
| `electron-builder.yml` | パッケージ設定 (NSIS インストーラ + ポータブル exe、asar 無効) |
| `.github/workflows/desktop.yml` | windows-latest で exe をビルドして artifact に上げる CI |
| `package.json` | `main: electron/main.mjs`、`app:*` スクリプト、electron/electron-builder devDeps |

## データ配置 (userData)

サーバは cwd 相対 (`./discutere.config.json`, `./data/*.db`, `./data/games` 等) で
パスを解決するため、アプリは起動時に **cwd を userData に固定**する
(Windows: `%APPDATA%/Discutere`)。初回起動時にリポ同梱の読み取りデータをシードする:

- `data/games`, `data/affects` — メカニクス md / affect 定義 (extraResources → `resources/seed/`)
- `worker-home` — worker-pool backend 用の作業ディレクトリ
- `discutere.config.example.json` → `discutere.config.json` — 設定の雛形 (**既存なら上書きしない**)

DB (SQLite / KG)・ログもすべて userData 配下に作られる。メニューの
「ファイル → データフォルダを開く / 設定ファイルを開く」から辿れる。
設定変更はアプリ再起動で反映。`DISCUTERE_CONFIG` env で別パスの config も指定可能。

Discord bot として使う場合は `discutere.config.json` に `discord.botToken` 等を記入する
(HTTP 公開は不要 — Gateway は WS 常時接続なのでローカル PC から外向き接続のみ)。
bot token 無しでも Web UI (`/flow` のチャット議論・学習ビュー) はローカルで動く。

## ビルド方法

### CI (推奨)

`.github/workflows/desktop.yml` が windows-latest で exe をビルドする。

- 手動: Actions → **Desktop (Windows exe)** → Run workflow
- タグ: `v*` を push (実配布はデータチューナー対応完了後の運用)

配布 (パッケージビルド) は基礎挙動の CI (`ci.yml`) と無関係なので **PR では自動実行しない**
(手動 dispatch かタグ push のみ)。

成果物 (artifact `discutere-windows-<sha>`):

- `Discutere-Setup-<version>-x64.exe` — NSIS インストーラ (per-user、管理者権限不要)
- `Discutere-Portable-<version>-x64.exe` — インストール不要の単一 exe

### ローカル (Windows マシン)

```bash
npm run setup:submodules # lib/{lapilli,canalis,vestigium,fundamentum} を init + build (初回のみ)
npm ci                    # @ludiars/* は全て file: submodule 参照。NODE_AUTH_TOKEN 不要
npm run app:dist:win     # build → app:rebuild → electron-builder --win
# 出力: release/*.exe
```

> **パブリッシュ (配布) の順序**: exe をビルドするだけならこの手順で足りるが、実際の
> リリース (`v*` タグ push → CI で成果物公開) はデータチューナー (Fundamentum 議論データ export、
> `docs/fundamentum-export.md` 参照) 対応完了後に行う運用。

開発時のウィンドウ確認は `npm run app:dev` (要: 事前に `npm run app:rebuild`)。

## ネイティブモジュールの ABI (重要)

- **kuzu**: N-API ビルドなので Node/Electron 両方でそのまま動く。リビルド不要
  (そもそも npm パッケージにソースが無いためリビルド不可)。`npmRebuild: false` の理由。
- **better-sqlite3**: V8 API 直依存なので **Electron の ABI 向けリビルドが必須**。
  `npm run app:rebuild` (`electron-rebuild -f -w better-sqlite3`) で行う。
  **リビルド後は素の Node (`npm start` / tsx) で better-sqlite3 が動かなくなる**ので、
  サーバ開発に戻るときは `npm run app:rebuild:undo` で Node ABI に戻すこと。

## 起動シーケンス / 失敗時

1. 単一インスタンスロック (2 重起動は既存ウィンドウへフォーカス)
2. userData シード → `DISCUTERE_CONFIG` 設定 → `process.chdir(userData)`
3. `dist/index.js` を dynamic import (サーバ起動、Discord token 無ければ gateway skip)
4. `/health` を最大 60 秒ポーリング → OK でトップページ表示

起動失敗 (EADDRINUSE 等) はエラーダイアログ + `userData/logs/discutere-desktop.log`。
console 出力は同ログに tee される。外部リンクは OS ブラウザで開く
(ウィンドウ内は loopback origin のみ)。終了はサーバごと落ちる (常駐しない —
常駐運用は従来どおり `npm start` / systemd 等)。

## 制約 / 注意

- `llm.backend=claude-cli` / `worker-pool` は PC に `claude` CLI が必要 (アプリには同梱しない)。
  ローカル LLM は `llm.backend=local` (Ollama 等) を推奨。
- macOS / Linux ターゲットは未設定 (必要になったら `electron-builder.yml` に `mac:`/`linux:` を足す)。
- 自動更新 (electron-updater) は未実装。配布は exe の手動差し替え。
- コード署名なし — Windows SmartScreen の警告が出る ("詳細情報 → 実行" で回避)。
