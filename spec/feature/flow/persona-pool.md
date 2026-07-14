# 匿名ペルソナプール — 設計

> 状態: canonical (2026-07-13)

## 目的

Discutere は匿名 workspace として、外部で形成された人物像を議論の観点に利用する。
人格の作成やアンケート回答の収集は行わず、同一人物のデータを安定した匿名キーで束ねる。

## 責務境界

- Voluptas: 認証、アンケート、プレイログ、ペルソナ生成、個人データの管理。
- Discutere: 匿名ペルソナの取込、公開レビュー話者の仮名化、20D affect 近傍選択、議論への憑依。
- Lapilli: 両サービスで共有する固定 20D sentiment-core。

Discutere に質問票生成、回答保存、回答からのペルソナ生成、ランダム合成、自己プロフィール作成を置かない。
議論を成立させるための使い捨てキャスト生成 (`src/flow/personas.ts`) は別責務なので維持する。

## Voluptas 取込契約

入力は JSON 配列、または `{ "personas": [...] }` とする。

```json
{
  "user_id": "ext:voluptas:0123456789abcdef",
  "affect_vector": [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0],
  "vector_spec_version": 1,
  "traits": ["探索型", "協力重視"]
}
```

- `user_id` はフィールド名を維持するが、値は Voluptas の内部 SID ではない。
- Voluptas が secret HMAC で作る `ext:voluptas:<16 hex>` の仮名 ID のみ受理する。
- 表示名、メール、OAuth provider ID、raw profile は契約に含めない。
- `affect_vector` は sentiment-core v1 と同じ固定 20 次元。次元・有限値・version を入口で検証する。
- `user_id` の unique indexで upsert し、再 export は同じ人物を更新する。
- Voluptas 発話 export の同じ HMAC ID は `ext:feedback:<user_id>` として `source_speaker_id` にも結び、
  レビュー話者採用との二重登録を防ぐ。affect profile は Voluptas を正本としてレビュー集計で上書きしない。
- UI 表示名は `user_id` 自体を出さず、別の一方向 hash から `プレイヤー#xxxxxxxx` を導出する。

取込方法:

```text
npm run persona:import -- --input <voluptas-personas.json>
```

loopback 管理画面 `/api/admin/personas` から同じ JSON を選ぶこともできる。

## 公開レビュー話者

Steam / YouTube 等の公開レビューは従来どおり `source_speaker_id` で同一人物を束ねる。
外部公開 ID は内部アンカーにだけ保持し、露出面では `論者#xxxxxx` に置き換える。
これは Voluptas `user_id` と同じ「同一人物確認のための安定キー」という扱いである。

## 旧機能の移行

`flow_0023_external_persona_identity` で次を行う。

- `flow_persona.user_id` と partial unique indexを追加。
- Di が生成した `origin=generated|synthesized` を論理アーカイブし、議論候補から外す。
- Di 固有の自己嗜好表 `flow_user_affect` を削除する。

旧 migration の母数推定列は SQLite の互換性のため残すが、実装からは参照しない。
