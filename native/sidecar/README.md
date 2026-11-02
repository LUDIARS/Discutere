# native/sidecar

C++ に移さず Node のまま残す機能を、C++ サーバから別プロセスで呼ぶための入口 (`di::sidecar::run`)。
各スクリプトは標準入力の JSON を 1 つ読み、標準出力に `{"ok":true,"result":...}` か `{"ok":false,"error":"..."}` を 1 行書く。
起動は `node --import tsx native/sidecar/<script>` (引数配列、シェル不使用)。

| スクリプト | 中身 | Node に残す理由 |
|---|---|---|
| `backup-archive.ts` | 学習データの tar.gz 作成 (`src/backup/archive.ts` の `createArchive` をそのまま呼ぶ)。C++ は `di::backup::create_archive` | tar 形式・gzip の作成を自作しない (`tar` パッケージ) |
| `spec-extract.ts` | 仕様書の本文抽出 (PDF = pdf-parse / DOCX = mammoth / PPTX・XLSX = ZIP 展開 + XML 抽出 (zlib) / 旧 DOC)。入力はバイト列 (base64) と方式名 (`spec.pdf` 等) と見出しだけで、ファイルパスは受け取らない。移植前の `src/flow/spec-source.ts` の `extractSpecTextFromBytes` をそのまま呼ぶので ZIP 展開の上限 (1 エントリ 64MiB・合計 128MiB、PR #3487) も同じ。呼び出し元 `native/src/flow/spec_source.cpp` (期限 120 秒・出力 32MiB・入力 64MiB) | 複雑な外部形式のパーサーを自作しない (pdf-parse / mammoth / zlib) |
| `canalis-crawl.ts` | Notion リンクのクロール (@ludiars/canalis の `NotionApiClient` + `crawlPage` / `crawlPublicPages`)。入力は `{url, depth (0..5), maxPages (1..200)}` だけで、URL は Canalis の `isNotionUrl` で検査し直す。token (env `NOTION_TOKEN`) は Node 側で読み、C++ には渡らない。取得手順と warn の文言は移植前の `src/flow/notion-link.ts` の `resolveNotionUrl` と同じ。出力はページ配列 (`url,title,depth,markdown`) と warn で、md への連結は C++ (`native/src/flow/notion_link.cpp`、期限 = 60 秒 + 30 秒 × ページ数 (上限 15 分)・出力 32MiB・stdin 16KiB) | ブラウザ操作 (playwright) と Notion API クライアントを自作しない |
| (各担当が追加) | | |

これらは npm 依存 (pdf-parse / mammoth / tar / @ludiars/canalis + playwright) を使う。
切り替え後もこの機能を使う間は、該当の npm 依存だけを残す。
