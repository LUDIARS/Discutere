# 09 — ペーパーゲート: 議論適性評価 + 論点分解の前倒し + フロー再提案

対応指摘: **C2** (評価が「量」のみで議論適性軸が欠落)・**C3** (論点分解が人間レビュー管轄外)。
フェーズ: **PR-D**。正本 spec: [dialectic.md §7](../../feature/flow/dialectic.md)。

## 問題

現行のペーパー評価は「情報ゲート = 量と網羅 (sparse/moderate/rich)」+「レビューゲート =
人間による体裁確認」の 2 軸のみ。情報が rich でも議論が成立しないテーマ (争点がない・
片側しか武装できない・証拠が偏っている) を検出できず、議論劇場になる。
また全体の質の上限を決める論点分解が、人間が確認できる場 (レビューゲート) の外にある。

## 設計

### 1. 議論適性 (debatability) チェック — 情報ゲートの後段に追加

`src/flow/debatability.ts` (新規、DB 非依存・DI でテスト可能)。3 検査:

| 検査 | 方法 | 出力 |
|---|---|---|
| **争点の存在** | LLM: テーマ + ペーパーから「賛否が本気で割れる論点」を 3〜5 個分解 (dialectic [0] と同一関数 `decomposeIssues` を共用) | issues[] (0〜5 個) |
| **両論武装可能性** | LLM (小モデル): issue ごとに「この材料で pro / con の両方が根拠を張れるか」を判定 | issue ごとの armable: both / pro-only / con-only / neither |
| **証拠バランス** | **機械計算** (LLM 不使用): 収集済み voices の極性分布を既存 20 次元ベクトル (`textToVector` の valence) で集計、出所 (source) の偏りも数える | { polaritySkew: -1..1, dominantSource: {name, share} } |

判定: `armable=both` の issue が **2 個未満** → **議論不適**。

### 2. フロー種別の再提案

議論不適のとき、ゲートは議論を強行せず**再提案**を出す:

- 争点 0〜1 個 (材料はある) → 「壁打ち」を提案
- 材料不足が主因 (polaritySkew 極端 / voices 僅少) → 「学習」を提案 (足りない側の観点を提示)
- Web: レビューゲート画面に提案カード (「このまま議論開始」も選べる — 人間が最終決定)
- Discord: スレッドに提案リプライ + 既存の議論タイプ選択メニュー再提示
- 強行時は `discussion_paper` に `debatability_json` を残し、結論に「議論適性: 低 (争点 n)」を併記

### 3. 論点分解のゲート前倒し (md 正本 `# 論点` 節)

- ペーパー草案生成 (`buildPaperDraft`) 時に `decomposeIssues` を走らせ、md 正本に節を追加:

```markdown
# 論点
1. ガチャ緩和は長期収益を毀損するか (両論: ○)
2. 天井導入はライト層の課金動機を下げるか (両論: ○)
3. ...
```

- **人間は既存の Notion 風ブロック編集で論点を追加・削除・言い換えできる** (追加 UI 不要。
  ブロック単位の LLM レビュー・戻す・承認がそのまま効く)。
- `markdownToPaperDraft` が `# 論点` 節を `issues[]` として復元し、承認時
  `runFlow(paperOverride)` / `runDialecticFlow` に渡す → dialectic エンジンは
  **承認済み論点を [0] の結果として採用** (再分解しない。[06](./06-dialectic-core.md) 参照)。
  rounds エンジンでは論点節はファシリテーター開幕プロンプトの参考情報として注入。
- `PaperOverride` に `issues?: string[]` を追加。`discussion_paper` に
  `debatability_json` 列 (migration。null 可)。

### 4. 情報ゲートとの関係

- 実行順: 情報ゲート (量) → **議論適性ゲート (質)** → 人間レビューゲート (承認)。
- 議論適性の再評価は人間が論点を編集した後の承認時には**走らせない** (人間の判断が最終)。
- `flow.debatability.enabled` (既定 true) / `minArmableIssues` (既定 2)。無効時は現行挙動。

## 実装ファイル

| ファイル | 変更 |
|---|---|
| `src/flow/debatability.ts` (新規) | 3 検査 + 判定 (DI 境界、単体テスト可) |
| `src/flow/dialectic/issues.ts` | `decomposeIssues` を共用 export ([06] と同一実装) |
| `src/flow/paper-review.ts` | 草案に論点節、debatability 結果の表示・再提案 |
| `src/flow/paper-markdown.ts` | `# 論点` 節の round-trip |
| `src/flow/director.ts` / `dispatch.ts` | `PaperOverride.issues` の受け渡し |
| `src/flow/web/routes.ts` / `discord-live.ts` | 再提案の露出 (カード / リプライ) |
| `src/config.ts` | `flow.debatability.{enabled, minArmableIssues}` |

## テスト / 受け入れ基準

- [ ] 争点 1 個のペーパーで「壁打ち」再提案が出る / 強行フラグで議論も開始できる
- [ ] `# 論点` 節が md round-trip で issues[] に復元され、dialectic [0] が再分解をスキップする
- [ ] 極性 9:1 の voices で polaritySkew が検出されゲート表示に出る (機械計算・LLM 不使用の単体テスト)
- [ ] `flow.debatability.enabled=false` で現行挙動と完全一致
- [ ] 新規 `tests/flow/debatability.test.ts` + paper round-trip テスト拡張
