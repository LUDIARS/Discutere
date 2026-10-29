# PPTX / XLSX の ZIP 展開上限

関連タスク: `actio:caf2f231-bb32-44f0-b3e1-362043faefd4`

`src/flow/spec-source.ts` の共通 ZIP 読み取りで、展開後の各エントリを
64 MiB 以下、全エントリの累積を 128 MiB 以下に制限する。大きな slide / sheet /
sharedStrings XML を許容しつつ、圧縮率に依存した無制限のメモリ確保を防ぐ。
圧縮前サイズの申告は信用せず、inflate 自体に各エントリ上限と合計残容量の小さい方を渡す。
非圧縮エントリにも同じ上限を適用し、重複名や本文以外のエントリも累積に含める。
上限と同じサイズおよび容量を使い切った後の空エントリは許容する。

URL・ローカルファイル・アップロードのいずれも共通処理を通る。
超過や展開失敗は既存形式の `label: ` で始まるエラーとし、部分的な抽出結果を返さない。
正常な PPTX / XLSX の抽出結果・並び順は維持する。PDF / DOCX の抽出には適用しない。

受け入れ契約（今回の run 用 manifest。過去タスクの述語ファイルは保持）:

C-1 inflateZipEntry(data, compression, maxBytes, label): 展開結果は64 MiBと合計残容量の両方を超えず、失敗はlabel付きで通知する

検証は `src/flow/spec-source.test.ts` のみ。正常な非圧縮／deflate Office XML、
64 MiB 境界、サイズ申告を偽った超過、128 MiB 境界／超過、重複名、空エントリ、
非圧縮エントリ、各入力経路、破損 deflate をメモリ上で生成した ZIP で確認する。
Augur の bug_fix 計画に従い、回帰テストを先に記述し、その後に制限を実装する。
