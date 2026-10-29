# native/web

ブラウザで動く画面 (HTML / CSS / ブラウザ用 JS)。C++ には移植しない。
移植前の TS (例 `src/flow/web/page.ts`) がサーバ側で組み立てていた文字列を、そのままファイルとして置く。
ビルド時に `cmake/EmbedWeb.cmake` が埋め込み、`di::web::find("<相対パス>")` で引ける。

サーバ側の値を差し込む画面は、TS と同じ位置に `{{名前}}` の印を置き、C++ 側で HTML エスケープして置き換える。
