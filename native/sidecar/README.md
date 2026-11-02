# native/sidecar

C++ に移さず Node のまま残す機能を、C++ サーバから別プロセスで呼ぶための入口 (`di::sidecar::run`)。
各スクリプトは標準入力の JSON を 1 つ読み、標準出力に `{"ok":true,"result":...}` か `{"ok":false,"error":"..."}` を 1 行書く。
起動は `node --import tsx native/sidecar/<script>` (引数配列、シェル不使用)。

| スクリプト | 中身 | Node に残す理由 |
|---|---|---|
| `backup-archive.ts` | 学習データの tar.gz 作成 (`src/backup/archive.ts` の `createArchive` をそのまま呼ぶ)。C++ は `di::backup::create_archive` | tar 形式・gzip の作成を自作しない (`tar` パッケージ) |
| (各担当が追加) | | |

これらは npm 依存 (pdf-parse / mammoth / tar / @ludiars/canalis + playwright) を使う。
切り替え後もこの機能を使う間は、該当の npm 依存だけを残す。
