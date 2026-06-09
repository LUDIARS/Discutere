# 進行役への調整指示 (facilitator directives)

Discord で **bot (@Discutere) に @メンション** して、進行中の議論の
**進め方・トーン・語り口** をその場で調整できる機能。

> 例: `@Discutere もう少し簡単な言葉で話して` / `@Discutere もっと否定的な意見が欲しいな`

議題そのもの (対象ゲーム/ジャンル) は変えない。 あくまで **議論の steer** に使う。
**persona への通常のリプライは「参加発言」として扱い、 調整指示にはしない**
(調整したいときは bot を明示メンションする)。

## 動作

1. **検知** (`src/discord-hook/gateway.ts`, `MessageCreate`):
   監視対象 (フォーラムスレッド / `discord.discussionChannelIds` のチャンネル・その子スレッド) で
   **bot (@Discutere) へのメンション**を「調整指示」として検知する。 通常の utterance 取り込みには
   **回さない**。 メンションの無い投稿 (persona へのリプライ含む) は従来どおり参加発言として取り込む。
2. **保存** (`directive-handler.ts` → `facilitator-directives.ts`):
   発話場所の scene `discord:<guild>/<channel>` から進行中の議論 (open な
   `discussion-of-gap` session) を引き、 その **design gap に紐付けて** 指示を保存する
   (サイドカー表 `facilitator_directives`)。 議論が立っていなければ保存せず案内文を返す。
3. **了解返信**: Discutere が「🛠️ 了解です。『…』 を進行に反映します」 と一言リプライする。
4. **注入** (以後の進行・発話に反映):
   - **facilitator** (`facilitator.ts` の `gapTopic`) — expand / converge / 止揚判定の
     全 prompt に「進行役への調整指示」 ブロックを差し込む。
   - **persona** (`engine/prompt-builder.ts`) — 議題ブロックの直後に同ブロックを差し込む。
   どちらも直近 5 件を時系列で載せる (古い指示で文脈が膨れないよう件数制限)。

## 設計メモ

- 保存は **gap 単位**。 別スレッド/別議論には混ざらない。 gap が closed/converged になれば
  scene 解決が外れるので、 閉じた議論には効かない。
- `author_id` は監査用に保存するが **prompt には出さない** (個人データを persona に渡さない、
  匿名 workspace 方針)。
- 認可は不要 (リアクション同様、 参加者が議論を調整するのは自然な参加行為)。
- サイドカー表は `aufhebung_stock` 等と同じく `CREATE TABLE IF NOT EXISTS` で冪等確保。

## 関連ファイル

| ファイル | 役割 |
|---|---|
| `src/discord-hook/facilitator-directives.ts` | ストア + scene→gap 解決 + prompt 整形 (純 DB) |
| `src/discord-hook/directive-handler.ts` | 取り込みオーケストレーション + ack 文生成 (transport 非依存) |
| `src/discord-hook/gateway.ts` | メンション/リプライ検知 → handler 呼び出し → リプライ |
| `src/persona-engine/facilitator/facilitator.ts` | `gapTopic` で facilitator prompt に注入 |
| `src/persona-engine/engine/prompt-builder.ts` | persona 発話 prompt に注入 |
| `src/discatier-engine-adapter/index.ts` | `listFacilitatorDirectives` の実装 (adapter) |
