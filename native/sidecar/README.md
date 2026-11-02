# native/sidecar

C++ に移さず Node のまま残す機能を、C++ サーバから別プロセスで呼ぶための入口 (`di::sidecar::run`)。
各スクリプトは標準入力の JSON を 1 つ読み、標準出力に `{"ok":true,"result":...}` か `{"ok":false,"error":"..."}` を 1 行書く。
起動は `node --import tsx native/sidecar/<script>` (引数配列、シェル不使用)。

| スクリプト | 中身 | Node に残す理由 |
|---|---|---|
| `spec-extract.ts` | 仕様書の本文抽出 (PDF = pdf-parse / DOCX = mammoth / PPTX・XLSX = ZIP 展開 + XML 抽出 (zlib) / 旧 DOC)。入力はバイト列 (base64) と方式名 (`spec.pdf` 等) と見出しだけで、ファイルパスは受け取らない。移植前の `src/flow/spec-source.ts` の `extractSpecTextFromBytes` をそのまま呼ぶので ZIP 展開の上限 (1 エントリ 64MiB・合計 128MiB、PR #3487) も同じ。呼び出し元 `native/src/flow/spec_source.cpp` (期限 120 秒・出力 32MiB・入力 64MiB) | 複雑な外部形式のパーサーを自作しない (pdf-parse / mammoth / zlib) |
| (各担当が追加) | | |

これらは npm 依存 (pdf-parse / mammoth / tar / @ludiars/canalis + playwright) を使う。
切り替え後もこの機能を使う間は、該当の npm 依存だけを残す。
