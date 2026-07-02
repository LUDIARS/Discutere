# 10 — コード衛生: runFlow 分割 + プロンプト重複解消

対応指摘: **A11** (runFlow 440 行)・**A12** (paperToPrompt 重複)。フェーズ: **PR-A**
(01〜04 と同一 PR。分割を先にやると 01〜04 の diff が小さくなるため、**PR-A 内で最初に着手**)。

## 問題

- `director.ts` の `runFlow` (207-643 行) が step 1〜9 をインラインで抱える。
  Ars 共通規約 (coding-conventions: SRP・ファイル分割必須) 違反。
- `discussion-paper.ts` の `paperToPrompt` (222-256) が `buildPaperSystem` +
  `buildPersonaUserPrompt` (165-216) と stanceLine ほかを丸ごと複製。片方の修正が
  もう片方に伝播しない事故の温床 (01 で stance 注入を変えるため先に潰す)。

## 設計

### 1. runFlow の分割

`runFlow` は「進行の台本」だけを残し、各 step を関数へ (すべて `src/flow/` 内・既存名を維持):

| 切り出し先 | 内容 (現行行) |
|---|---|
| `flow-setup.ts` (新規) | [1] investigate + 増補 + [2] ペーパー初期化 (250-316) |
| `facilitator-turn.ts` (新規) | [4] 開幕ターン (367-419)。02 の文脈注入もここ |
| `persona-turn.ts` (新規) | [5] 1 ターン実行 (422-504): paper 組み立て → invoke → record 化 |
| `director.ts` (残) | ループ制御・評価・サマリ・収束判定・結論の呼び出し順のみ (~150 行目標) |

- 挙動変更なし (リファクタのみ)。onUtterance / persistUtterance の呼び出し順序を変えない。
- 各関数は deps を引数で受ける (既存の DI スタイル踏襲。テストで Mock 注入可能に)。

### 2. paperToPrompt の重複解消

```ts
export function paperToPrompt(p, stance, persona): string {
  return [buildPersonaUserPrompt(p, stance, persona), "", "---", buildPaperSystem(p)].join("\n");
}
```

- 単一文字列が要る後方互換呼び出しのために**合成で残す** (複製コードは削除)。
  出力バイトが変わるため、既存利用箇所 (grep して sparring / persona-survey 等) の
  テスト期待値を追従させる。利用箇所ゼロなら削除してよい (grep で確認の上判断)。
- stanceLine の switch は `stanceLine(stance): string` として 1 箇所に。

## テスト / 受け入れ基準

- [ ] リファクタ後、既存全テスト (`npm run test:*`) が無修正 or 期待値追従のみで通る
- [ ] `runFlow` 本体が ~150 行以内 (ループ台本のみ)
- [ ] stanceLine の定義が 1 箇所 (grep で複製ゼロ)
- [ ] `npm run build` (tsc) 通過
