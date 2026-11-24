# creator-insight 集計ツールの索引

`skills/creator-insight/references/tools.md` の決まりで作ったツールの一覧。作る前にここを見て、同じ入力と出力のものがあれば使う。

| 名前 | 入力 | 出力 | 作成日 |
|---|---|---|---|
| `life-script-items` | 設問 JSON (省略可) | チャットで 1 問ずつ出す順番に並べた設問 (尺度名・逆転は出さない) | 2026-11-24 |
| `score-life-script` | 回答 JSON、設問 JSON (省略可) | 尺度得点、優位、確度、偏りの検査 (`{ lifeScript }`、特徴カードと同じ形) | 2026-11-24 |
| `plan-life-script-followups` | 採点結果 JSON、追加質問 JSON (省略可)、設問 JSON (省略可) | 追加質問の話題・最初の問い・根拠 (`{ followups }`) | 2026-11-24 |

- 共通部品: `lib/questionnaire.ts` (設問 JSON の読み込みと形の検査、入力の上限 256KB)。
- 実行: `npm run insight:<名前> -- <引数>`。テスト: `npm run test:creator-insight` (架空データのみ)。
- 依存は標準ライブラリだけ。ネットワークに出ない。LLM を呼ばない。
