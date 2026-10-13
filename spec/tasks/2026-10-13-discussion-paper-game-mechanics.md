# 議論ペーパーにゲーム名一致のメカニクスを載せる

- Actio: `actio:465c8466-aa85-4996-8b54-300086609094`
- 仕様: [`spec/feature/flow/discussion.md` §2-0](../feature/flow/discussion.md)
- ブランチ: `feat/discussion-paper-game-mechanics`

## 分解

1. [x] `loadThemeMechanics` を `src/flow/investigate.ts` から公開する (従来の内部関数 `loadMechanics`)。
2. [x] `buildPaperDraft` (flow=discussion) でゲーム名 (`seed.gameTitle`、無ければ議題) に一致する
   data/games のメカニクスを読む。YouTube 収集と LLM 増補はしない。改善フローは変更なし。
3. [x] 単体テスト `tests/flow/paper-review-discussion-mechanics.test.ts`
   (一致で載る・本文 md に出る / 不一致は空 / youtubeSearch と増補 LLM が呼ばれない / 事前情報が先勝ち)。
4. [x] 仕様 (discussion.md §2-0) と CLAUDE.md の追記。
5. [x] Augur 契約 C-5 (`loadThemeMechanics`) / C-6 (`buildPaperDraft`) を追加。
   この repo には `augur.inject.json` が無いため inject はできない (既知の制約)。
