# 人物パック契約 v1 (Histrio / proxy:form → Discutere)

> 状態: canonical (2026-10-23)。検証の実装は `src/flow/proxy/contract.ts`。
> 機能仕様は `spec/feature/flow/proxy-debate.md`。

1 要素 = 1 話者 × 1 ジャンル。JSON 配列 / `{ "packs": [...] }` / JSONL で渡す。

```jsonc
{
  "contractVersion": 1,
  "personaKind": "observedSpeaker",
  "speakerKey": "ext:steam:3f9a0c1d2e4b5a6f",   // ext:<source>:<hmac16>。生 ID・表示名を入れない
  "histrioPersonaId": "steam-3f9a0c1d2e4b5a6f", // 任意。Steam 代理の Histrio 上の仮名 ID
  "genre": "roguelike",                          // 形成側のジャンル辞書 id (英小文字・数字・-_)
  "genreSpecVersion": 1,
  "profileVersion": "proxy-form-v1:0a1b2c3d4e5f",
  "formation": { "method": "rule", "ruleVersion": "proxy-form-v1" },   // rule | llm
  "window": { "observedMonths": null, "generatedAt": 0, "expiresAt": 0 }, // expiresAt > generatedAt
  "sufficiency": { "utterances": 12, "positive": 7, "negative": 5, "games": 4, "aspects": 4,
                   "confidence": "medium", "reasons": [] },              // low はキャストに出さない
  "affect": { "vector": [/* 20 */], "vectorSpecVersion": 1 },          // sentiment-core v1
  "designGap": { "vector": [/* 20 */] /* or null */, "method": "pos-minus-neg",
                 "wants": ["asp.difficulty"], "complaints": ["asp.content"], "reason": null },
  "conditionalGaps": [ { "current": "...", "desired": "...", "condition": null, "evidenceIds": ["E1"] } ],
  "stanceCards": [ { "id": "S1", "claim": "...", "polarity": "neg", "evidenceIds": ["E1"], "extractor": "rule" } ],
  "evidence": [ { "id": "E1", "revision": "...", "text": "≤400 字", "sourceKind": "steam",
                  "gameTitle": "...", "polarity": "neg", "postedAt": null } ],   // 1〜12 件
  "styleNotes": ["短く書く"],                    // ≤ 4
  "likedGames": ["..."],
  "heldOut": [ { "game": "...", "recommended": true } ],   // 評価用。本文は別の評価専用ファイル
  "revoked": false
}
```

Steam 代理では、任意の `histrioPersonaId` に Histrio 上の既存の仮名 ID を指定できる。形式は `steam-[A-Za-z0-9_.-]{1,122}`。Di のキャスト ID と `speakerKey` はパックと議論を結び付け、Histrio のプロフィール照会には `histrioPersonaId` を使う。ID やプロフィールが無い場合は脚本を省略する。代理の論拠は人物パックを使う。Histrio 側の Steam 人格取り込みは別タスク。

## 拒否する条件 (Di 入口)

- `speakerKey` が仮名の形でない / `contractVersion` ≠ 1 / `vectorSpecVersion` ≠ 1 / 20 次元でない
- どの階層でも `url` `sourceUrl` `authorId` `authorName` `displayName` `steamId` `userId` 等のフィールドがある
- どの文字列にも `http(s)://` を含む (本文中のリンクも不可)
- 立場カード・条件付き gap の `evidenceIds` が空、またはこのパックの `evidence` に無い id を指す
- `designGap.vector` が null なのに `reason` が無い

## 失効

`{ "contractVersion": 1, "speakerKey": "...", "genre": "...", "revoked": true }` の最小形でも受け付け、
その人物 × ジャンルを以後のキャストから外す。同じ内容のパックを再提供すると失効が解ける。

## 評価専用ファイル (heldOut 本文)

`proxy:form --heldout-out` が出す JSONL。`{ speakerKey, genre, game, text }`。`eval:proxy --heldout` だけが読む。
取込・束・議論には渡さない。
