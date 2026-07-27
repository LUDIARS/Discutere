# ローカル Vo 連携・仕様施策議論

## フロー

1. `/flow` で「自由入力」「議論仕様書」「施策リスト」のいずれかを選ぶ。
2. 必要なら Vo ローカルペルソナを取得し、当事者視点としてペーパーへ追加する。
3. Anatomia を使うか明示する。使用時は既定で LLM を使わず、CLI のドメイン解析結果を決定論的に整理する。
4. ペーパーを確認して議論を開始する。
5. 結論の後、AI が「情報充足度」と「議論の有意味性」を各 100 点で採点する。

Vo の接続先ポートは Discutere に保持しない。Excubitor catalog / ProcessMap 由来の topology 環境変数
`VOLPUTAS_URL` を使用する。ローカル連携では loopback URL だけを受理する。

## ペルソナ交換型

単一取得の正本は JSON の `ludiars.persona-discussion-snapshot` とする。複数人・履歴の一括交換が
必要な場合だけ、同じオブジェクトを 1 行 1 snapshot の JSONL にする。

```json
{
  "schema": "ludiars.persona-discussion-snapshot",
  "schemaVersion": 1,
  "subject": { "kind": "local-self" },
  "source": {
    "service": "volputas",
    "modelVersion": "evidence-persona-v1",
    "analyzedAt": "2026-07-27T00:00:00.000Z",
    "freshness": "current"
  },
  "facets": [
    {
      "key": "volputas.evidence-persona.axes",
      "dimensions": [
        {
          "key": "exploration",
          "label": "探索志向",
          "score": 80,
          "scale": { "min": 0, "max": 100 },
          "evidenceWeight": 2
        }
      ]
    }
  ],
  "evidenceSummary": {
    "total": 10,
    "byType": { "surveys": 1, "gameplay": 2, "voices": 3, "emotionCurves": 4 }
  },
  "notes": ["入力済みデータからの推定です。"]
}
```

- `facets[].key` を名前空間として扱い、Vo の8軸と Di の20次元 affectを変換・混在させない。
- `dimensions` は固定長タプルではなくタグ付き配列にし、軸追加を後方互換にする。
- 生の回答、氏名、メール、GitHub名は議論用 snapshot に含めない。
- オンラインで複数ユーザを扱う場合、`subject` は生IDではなく既存の HMAC 仮名参照へ拡張する。
- snapshot は読み取り専用の分析結果であり、Vo の内部DBレコードをサービス契約にしない。

## ペルソナ生成の責務

個人データから長期的な人物像を生成する処理は Vo を正本にする。

- Voへ寄せる: `persona-adopt` 相当の実在話者集約、アンケート嗜好集計、ゲームプレイ・声・感情曲線の統合、
  入力fingerprintによる再分析判定。
- Diに残す: `personas.ts` の賛成/反対/司会キャスト、`persona-setup.ts` の議論単位の価値軸・核主張、
  snapshotをペーパー文脈へ変換するアダプタ。
- Diの永続ペルソナプールは移行期間の互換入口とし、新しい個人ペルソナをDi自身では生成しない。

この分離により、Voでデータが増えた時だけsnapshotを再生成すればよく、Diは分析アルゴリズムや
Vo内部スキーマの更新に追従せずに済む。
